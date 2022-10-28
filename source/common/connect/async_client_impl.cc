#include "source/common/connect/async_client_impl.h"

#include "envoy/config/core/v3/connect_service.pb.h"

#include "source/common/buffer/zero_copy_input_stream_impl.h"
#include "source/common/common/enum_to_int.h"
#include "source/common/common/utility.h"
#include "source/common/connect/common.h"
#include "source/common/http/header_map_impl.h"
#include "source/common/http/utility.h"

#include "absl/strings/str_cat.h"

namespace Envoy {
namespace Connect {

AsyncClientImpl::AsyncClientImpl(Upstream::ClusterManager& cm,
                                 const envoy::config::core::v3::ConnectService& config,
                                 TimeSource& time_source)
    : cm_(cm), remote_cluster_name_(config.envoy_connect().cluster_name()),
      host_name_(config.envoy_connect().authority()), time_source_(time_source),
      metadata_parser_(Router::HeaderParser::configure(
          config.initial_metadata(),
          envoy::config::core::v3::HeaderValueOption::OVERWRITE_IF_EXISTS_OR_ADD)) {}

AsyncClientImpl::~AsyncClientImpl() {
  ASSERT(isThreadSafe());
  while (!active_streams_.empty()) {
    active_streams_.front()->resetStream();
  }
}

AsyncRequest* AsyncClientImpl::sendRaw(absl::string_view service_full_name,
                                       absl::string_view method_name, Buffer::InstancePtr&& request,
                                       RawAsyncRequestCallbacks& callbacks,
                                       Tracing::Span& parent_span,
                                       const Http::AsyncClient::RequestOptions& options) {
  ASSERT(isThreadSafe());
  auto* const async_request = new AsyncRequestImpl(
      *this, service_full_name, method_name, std::move(request), callbacks, parent_span, options);
  AsyncStreamImplPtr connect_stream{async_request};

  connect_stream->initialize(true);
  if (connect_stream->hasResetStream()) {
    return nullptr;
  }

  LinkedList::moveIntoList(std::move(connect_stream), active_streams_);
  return async_request;
}

RawAsyncStream* AsyncClientImpl::startRaw(absl::string_view service_full_name,
                                          absl::string_view method_name,
                                          RawAsyncStreamCallbacks& callbacks,
                                          const Http::AsyncClient::StreamOptions& options) {
  ASSERT(isThreadSafe());
  auto connect_stream =
      std::make_unique<AsyncStreamImpl>(*this, service_full_name, method_name, callbacks, options);

  connect_stream->initialize(options.buffer_body_for_retry);
  if (connect_stream->hasResetStream()) {
    return nullptr;
  }

  LinkedList::moveIntoList(std::move(connect_stream), active_streams_);
  return active_streams_.front().get();
}

AsyncStreamImpl::AsyncStreamImpl(AsyncClientImpl& parent, absl::string_view service_full_name,
                                 absl::string_view method_name, RawAsyncStreamCallbacks& callbacks,
                                 const Http::AsyncClient::StreamOptions& options)
    : parent_(parent), service_full_name_(service_full_name), method_name_(method_name),
      callbacks_(callbacks), options_(options) {}

void AsyncStreamImpl::initialize(bool buffer_body_for_retry) {
  const auto thread_local_cluster = parent_.cm_.getThreadLocalCluster(parent_.remote_cluster_name_);
  if (thread_local_cluster == nullptr) {
    callbacks_.onRemoteClose(Status::WellKnownConnectStatus::Unavailable, "Cluster not available");
    http_reset_ = true;
    return;
  }

  auto& http_async_client = thread_local_cluster->httpAsyncClient();
  dispatcher_ = &http_async_client.dispatcher();
  stream_ = http_async_client.start(*this, options_.setBufferBodyForRetry(buffer_body_for_retry));

  if (stream_ == nullptr) {
    callbacks_.onRemoteClose(Status::WellKnownConnectStatus::Unavailable, EMPTY_STRING);
    http_reset_ = true;
    return;
  }

  // TODO(htuch): match Buf Connect base64 encoding behavior for *-bin headers, see
  // https://github.com/envoyproxy/envoy/pull/2444#discussion_r163914459.
  headers_message_ = Common::prepareHeaders(
      parent_.host_name_.empty() ? parent_.remote_cluster_name_ : parent_.host_name_,
      service_full_name_, method_name_, options_.timeout);
  // Fill service-wide initial metadata.
  // TODO(cpakulski): Find a better way to access requestHeaders after runtime guard
  // envoy_reloadable_features_unified_header_formatter runtime guard is deprecated and
  // request headers are not stored in stream_info.
  // Maybe put it to parent_context?
  // Since request headers may be empty, consider using Envoy::OptRef.
  parent_.metadata_parser_->evaluateHeaders(headers_message_->headers(),
                                            options_.parent_context.stream_info);

  callbacks_.onCreateInitialMetadata(headers_message_->headers());
  stream_->sendHeaders(headers_message_->headers(), false);
}

// TODO(htuch): match Buf Connect base64 encoding behavior for *-bin headers, see
// https://github.com/envoyproxy/envoy/pull/2444#discussion_r163914459.
void AsyncStreamImpl::onHeaders(Http::ResponseHeaderMapPtr&& headers, bool end_stream) {
  const auto http_response_status = Http::Utility::getResponseStatus(*headers);
  const auto connect_status = Common::getConnectStatus(*headers);
  callbacks_.onReceiveInitialMetadata(end_stream ? Http::ResponseHeaderMapImpl::create()
                                                 : std::move(headers));
  if (http_response_status != enumToInt(Http::Code::OK)) {
    // https://github.com/connect/connect/blob/master/doc/http-connect-status-mapping.md requires that
    // connect-status be used if available.
    if (end_stream && connect_status) {
      // Due to headers/trailers type differences we need to copy here. This is an uncommon case but
      // we can potentially optimize in the future.

      // TODO(mattklein123): clang-tidy is showing a use after move when passing to
      // onReceiveInitialMetadata() above. This looks like an actual bug that I will fix in a
      // follow up.
      onTrailers(Http::createHeaderMap<Http::ResponseTrailerMapImpl>(*headers));
      return;
    }
    // Status is translated via Utility::httpToConnectStatus per
    // https://github.com/connect/connect/blob/master/doc/http-connect-status-mapping.md
    streamError(Utility::httpToConnectStatus(http_response_status));
    return;
  }
  if (end_stream) {
    // Due to headers/trailers type differences we need to copy here. This is an uncommon case but
    // we can potentially optimize in the future.
    onTrailers(Http::createHeaderMap<Http::ResponseTrailerMapImpl>(*headers));
  }
}

void AsyncStreamImpl::onData(Buffer::Instance& data, bool end_stream) {
  decoded_frames_.clear();
  if (!decoder_.decode(data, decoded_frames_)) {
    streamError(Status::WellKnownConnectStatus::Internal);
    return;
  }

  for (auto& frame : decoded_frames_) {
    if (frame.length_ > 0 && frame.flags_ != CONNECT_FH_DEFAULT) {
      streamError(Status::WellKnownConnectStatus::Internal);
      return;
    }
    if (!callbacks_.onReceiveMessageRaw(frame.data_ ? std::move(frame.data_)
                                                    : std::make_unique<Buffer::OwnedImpl>())) {
      streamError(Status::WellKnownConnectStatus::Internal);
      return;
    }
  }

  if (end_stream) {
    streamError(Status::WellKnownConnectStatus::Unknown);
  }
}

// TODO(htuch): match Buf Connect base64 encoding behavior for *-bin headers, see
// https://github.com/envoyproxy/envoy/pull/2444#discussion_r163914459.
void AsyncStreamImpl::onTrailers(Http::ResponseTrailerMapPtr&& trailers) {
  auto connect_status = Common::getConnectStatus(*trailers);
  const std::string connect_message = Common::getConnectMessage(*trailers);
  callbacks_.onReceiveTrailingMetadata(std::move(trailers));
  if (!connect_status) {
    connect_status = Status::WellKnownConnectStatus::Unknown;
  }
  callbacks_.onRemoteClose(connect_status.value(), connect_message);
  cleanup();
}

void AsyncStreamImpl::streamError(Status::ConnectStatus connect_status, const std::string& message) {
  callbacks_.onReceiveTrailingMetadata(Http::ResponseTrailerMapImpl::create());
  callbacks_.onRemoteClose(connect_status, message);
  resetStream();
}

void AsyncStreamImpl::onComplete() {
  // No-op since stream completion is handled within other callbacks.
}

void AsyncStreamImpl::onReset() {
  if (http_reset_) {
    return;
  }

  http_reset_ = true;
  streamError(Status::WellKnownConnectStatus::Internal);
}

void AsyncStreamImpl::sendMessage(const Protobuf::Message& request, bool end_stream) {
  stream_->sendData(*Common::serializeToConnectFrame(request), end_stream);
}

void AsyncStreamImpl::sendMessageRaw(Buffer::InstancePtr&& buffer, bool end_stream) {
  Common::prependConnectFrameHeader(*buffer);
  stream_->sendData(*buffer, end_stream);
}

void AsyncStreamImpl::closeStream() {
  Buffer::OwnedImpl empty_buffer;
  stream_->sendData(empty_buffer, true);
}

void AsyncStreamImpl::resetStream() { cleanup(); }

void AsyncStreamImpl::cleanup() {
  if (!http_reset_) {
    http_reset_ = true;
    stream_->reset();
  }

  // This will destroy us, but only do so if we are actually in a list. This does not happen in
  // the immediate failure case.
  if (LinkedObject<AsyncStreamImpl>::inserted()) {
    ASSERT(dispatcher_->isThreadSafe());
    dispatcher_->deferredDelete(
        LinkedObject<AsyncStreamImpl>::removeFromList(parent_.active_streams_));
  }
}

AsyncRequestImpl::AsyncRequestImpl(AsyncClientImpl& parent, absl::string_view service_full_name,
                                   absl::string_view method_name, Buffer::InstancePtr&& request,
                                   RawAsyncRequestCallbacks& callbacks, Tracing::Span& parent_span,
                                   const Http::AsyncClient::RequestOptions& options)
    : AsyncStreamImpl(parent, service_full_name, method_name, *this, options),
      request_(std::move(request)), callbacks_(callbacks) {

  current_span_ =
      parent_span.spawnChild(Tracing::EgressConfig::get(),
                             absl::StrCat("async ", service_full_name, ".", method_name, " egress"),
                             parent.time_source_.systemTime());
  current_span_->setTag(Tracing::Tags::get().UpstreamCluster, parent.remote_cluster_name_);
  current_span_->setTag(Tracing::Tags::get().UpstreamAddress, parent.host_name_.empty()
                                                                  ? parent.remote_cluster_name_
                                                                  : parent.host_name_);
  current_span_->setTag(Tracing::Tags::get().Component, Tracing::Tags::get().Proxy);
}

void AsyncRequestImpl::initialize(bool buffer_body_for_retry) {
  AsyncStreamImpl::initialize(buffer_body_for_retry);
  if (this->hasResetStream()) {
    return;
  }
  this->sendMessageRaw(std::move(request_), true);
}

void AsyncRequestImpl::cancel() {
  current_span_->setTag(Tracing::Tags::get().Status, Tracing::Tags::get().Canceled);
  current_span_->finishSpan();
  this->resetStream();
}

void AsyncRequestImpl::onCreateInitialMetadata(Http::RequestHeaderMap& metadata) {
  current_span_->injectContext(metadata, nullptr);
  callbacks_.onCreateInitialMetadata(metadata);
}

void AsyncRequestImpl::onReceiveInitialMetadata(Http::ResponseHeaderMapPtr&&) {}

bool AsyncRequestImpl::onReceiveMessageRaw(Buffer::InstancePtr&& response) {
  response_ = std::move(response);
  return true;
}

void AsyncRequestImpl::onReceiveTrailingMetadata(Http::ResponseTrailerMapPtr&&) {}

void AsyncRequestImpl::onRemoteClose(Connect::Status::ConnectStatus status, const std::string& message) {
  current_span_->setTag(Tracing::Tags::get().ConnectStatusCode, std::to_string(status));

  if (status != Connect::Status::WellKnownConnectStatus::Ok) {
    current_span_->setTag(Tracing::Tags::get().Error, Tracing::Tags::get().True);
    callbacks_.onFailure(status, message, *current_span_);
  } else if (response_ == nullptr) {
    current_span_->setTag(Tracing::Tags::get().Error, Tracing::Tags::get().True);
    callbacks_.onFailure(Status::Internal, EMPTY_STRING, *current_span_);
  } else {
    callbacks_.onSuccessRaw(std::move(response_), *current_span_);
  }

  current_span_->finishSpan();
}

} // namespace Connect
} // namespace Envoy
