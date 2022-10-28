#include "source/common/connect/buf_async_client_impl.h"

#include "envoy/config/core/v3/connect_service.pb.h"
#include "envoy/stats/scope.h"

#include "source/common/common/base64.h"
#include "source/common/common/empty_string.h"
#include "source/common/common/lock_guard.h"
#include "source/common/config/datasource.h"
#include "source/common/connect/common.h"
#include "source/common/connect/buf_connect_creds_impl.h"
#include "source/common/connect/buf_connect_utils.h"
#include "source/common/router/header_parser.h"
#include "source/common/tracing/http_tracer_impl.h"

#include "absl/strings/str_cat.h"
#include "connectpp/support/proto_buffer_reader.h"

namespace Envoy {
namespace Connect {
namespace {
static constexpr int DefaultBufferLimitBytes = 1024 * 1024;
}

BufAsyncClientThreadLocal::BufAsyncClientThreadLocal(Api::Api& api)
    : completion_thread_(api.threadFactory().createThread([this] { completionThread(); },
                                                          Thread::Options{"ConnectGoogClient"})) {}

BufAsyncClientThreadLocal::~BufAsyncClientThreadLocal() {
  // Force streams to shutdown and invoke TryCancel() to start the drain of
  // pending op. If we don't do this, Shutdown() below can jam on pending ops.
  // This is also required to satisfy the contract that once Shutdown is called,
  // streams no longer queue any additional tags.
  for (auto it = streams_.begin(); it != streams_.end();) {
    // resetStream() may result in immediate unregisterStream() and erase(),
    // which would invalidate the iterator for the current element, so make sure
    // we point to the next one first.
    (*it++)->resetStream();
  }
  cq_.Shutdown();
  ENVOY_LOG(debug, "Joining completionThread");
  completion_thread_->join();
  ENVOY_LOG(debug, "Joined completionThread");
  // Ensure that we have cleaned up all orphan streams, now that CQ is gone.
  while (!streams_.empty()) {
    (*streams_.begin())->onCompletedOps();
  }
}

void BufAsyncClientThreadLocal::completionThread() {
  ENVOY_LOG(debug, "completionThread running");
  void* tag;
  bool ok;
  while (cq_.Next(&tag, &ok)) {
    const auto& buf_async_tag = *reinterpret_cast<BufAsyncTag*>(tag);
    const BufAsyncTag::Operation op = buf_async_tag.op_;
    BufAsyncStreamImpl& stream = buf_async_tag.stream_;
    ENVOY_LOG(trace, "completionThread CQ event {} {}", op, ok);
    Thread::LockGuard lock(stream.completed_ops_lock_);

    // It's an invariant that there must only be one pending post for arbitrary
    // length completed_ops_, otherwise we can race in stream destruction, where
    // we process multiple events in onCompletedOps() but have only partially
    // consumed the posts on the dispatcher.
    // TODO(htuch): This may result in unbounded processing on the silo thread
    // in onCompletedOps() in extreme cases, when we emplace_back() in
    // completionThread() at a high rate, consider bounding the length of such
    // sequences if this behavior becomes an issue.
    if (stream.completed_ops_.empty()) {
      stream.dispatcher_.post([&stream] { stream.onCompletedOps(); });
    }
    stream.completed_ops_.emplace_back(op, ok);
  }
  ENVOY_LOG(debug, "completionThread exiting");
}

BufAsyncClientImpl::BufAsyncClientImpl(Event::Dispatcher& dispatcher,
                                             BufAsyncClientThreadLocal& tls,
                                             BufStubFactory& stub_factory,
                                             Stats::ScopeSharedPtr scope,
                                             const envoy::config::core::v3::ConnectService& config,
                                             Api::Api& api, const StatNames& stat_names)
    : dispatcher_(dispatcher), tls_(tls), stat_prefix_(config.buf_connect().stat_prefix()),
      target_uri_(config.buf_connect().target_uri()), scope_(scope),
      per_stream_buffer_limit_bytes_(PROTOBUF_GET_WRAPPED_OR_DEFAULT(
          config.buf_connect(), per_stream_buffer_limit_bytes, DefaultBufferLimitBytes)),
      metadata_parser_(Router::HeaderParser::configure(
          config.initial_metadata(),
          envoy::config::core::v3::HeaderValueOption::OVERWRITE_IF_EXISTS_OR_ADD)) {
  // We rebuild the channel each time we construct the channel. It appears that the Connect library is
  // smart enough to do connection pooling and reuse with identical channel args, so this should
  // have comparable overhead to what we are doing in Connect::AsyncClientImpl, i.e. no expensive
  // new connection implied.
  std::shared_ptr<connect::Channel> channel = BufConnectUtils::createChannel(config, api);
  // Get state with try_to_connect = true to try connection at channel creation.
  // This is for initializing Connect channel at channel creation. This GetState(true) is used to poke
  // the Connect lb at channel creation, it doesn't have any effect no matter it succeeds or fails. But
  // it helps on initialization. Otherwise, the channel establishment still happens at the first
  // request, no matter when we create the channel.
  channel->GetState(true);
  stub_ = stub_factory.createStub(channel);
  scope_->counterFromStatName(stat_names.buf_connect_client_creation_).inc();
  // Initialize client stats.
  // TODO(jmarantz): Capture these names in async_client_manager_impl.cc and
  // pass in a struct of StatName objects so we don't have to take locks here.
  stats_.streams_total_ = &scope_->counterFromStatName(stat_names.streams_total_);
  for (uint32_t i = 0; i <= Status::WellKnownConnectStatus::MaximumKnown; ++i) {
    stats_.streams_closed_[i] = &scope_->counterFromStatName(stat_names.streams_closed_[i]);
  }
}

BufAsyncClientImpl::~BufAsyncClientImpl() {
  ASSERT(isThreadSafe());
  ENVOY_LOG(debug, "Client teardown, resetting streams");
  while (!active_streams_.empty()) {
    active_streams_.front()->resetStream();
  }
}

AsyncRequest* BufAsyncClientImpl::sendRaw(absl::string_view service_full_name,
                                             absl::string_view method_name,
                                             Buffer::InstancePtr&& request,
                                             RawAsyncRequestCallbacks& callbacks,
                                             Tracing::Span& parent_span,
                                             const Http::AsyncClient::RequestOptions& options) {
  ASSERT(isThreadSafe());
  auto* const async_request = new BufAsyncRequestImpl(
      *this, service_full_name, method_name, std::move(request), callbacks, parent_span, options);
  BufAsyncStreamImplPtr connect_stream{async_request};

  connect_stream->initialize(true);
  if (connect_stream->callFailed()) {
    return nullptr;
  }

  LinkedList::moveIntoList(std::move(connect_stream), active_streams_);
  return async_request;
}

RawAsyncStream* BufAsyncClientImpl::startRaw(absl::string_view service_full_name,
                                                absl::string_view method_name,
                                                RawAsyncStreamCallbacks& callbacks,
                                                const Http::AsyncClient::StreamOptions& options) {
  ASSERT(isThreadSafe());
  auto connect_stream = std::make_unique<BufAsyncStreamImpl>(*this, service_full_name, method_name,
                                                             callbacks, options);

  connect_stream->initialize(false);
  if (connect_stream->callFailed()) {
    return nullptr;
  }

  LinkedList::moveIntoList(std::move(connect_stream), active_streams_);
  return active_streams_.front().get();
}

BufAsyncStreamImpl::BufAsyncStreamImpl(BufAsyncClientImpl& parent,
                                             absl::string_view service_full_name,
                                             absl::string_view method_name,
                                             RawAsyncStreamCallbacks& callbacks,
                                             const Http::AsyncClient::StreamOptions& options)
    : parent_(parent), tls_(parent_.tls_), dispatcher_(parent_.dispatcher_), stub_(parent_.stub_),
      service_full_name_(service_full_name), method_name_(method_name), callbacks_(callbacks),
      options_(options) {}

BufAsyncStreamImpl::~BufAsyncStreamImpl() {
  ENVOY_LOG(debug, "BufAsyncStreamImpl destruct");
}

BufAsyncStreamImpl::PendingMessage::PendingMessage(Buffer::InstancePtr request, bool end_stream)
    : buf_(BufConnectUtils::makeByteBuffer(std::move(request))), end_stream_(end_stream) {}

// TODO(htuch): figure out how to propagate "this request should be buffered for
// retry" bit to Buf Connect library.
void BufAsyncStreamImpl::initialize(bool /*buffer_body_for_retry*/) {
  parent_.stats_.streams_total_->inc();
  gpr_timespec abs_deadline =
      options_.timeout
          ? gpr_time_add(gpr_now(GPR_CLOCK_REALTIME),
                         gpr_time_from_millis(options_.timeout.value().count(), GPR_TIMESPAN))
          : gpr_inf_future(GPR_CLOCK_REALTIME);
  ctxt_.set_deadline(abs_deadline);
  // Fill service-wide initial metadata.
  auto initial_metadata = Http::RequestHeaderMapImpl::create();
  // TODO(cpakulski): Find a better way to access requestHeaders after runtime guard
  // envoy_reloadable_features_unified_header_formatter runtime guard is deprecated
  // and request headers are not stored in stream_info.
  // Maybe put it to parent_context?
  parent_.metadata_parser_->evaluateHeaders(*initial_metadata, options_.parent_context.stream_info);
  callbacks_.onCreateInitialMetadata(*initial_metadata);
  initial_metadata->iterate([this](const Http::HeaderEntry& header) {
    ctxt_.AddMetadata(std::string(header.key().getStringView()),
                      std::string(header.value().getStringView()));
    return Http::HeaderMap::Iterate::Continue;
  });
  // Invoke stub call.
  rw_ = parent_.stub_->PrepareCall(&ctxt_, "/" + service_full_name_ + "/" + method_name_,
                                   &parent_.tls_.completionQueue());
  if (rw_ == nullptr) {
    notifyRemoteClose(Status::WellKnownConnectStatus::Unavailable, nullptr, EMPTY_STRING);
    call_failed_ = true;
    return;
  }
  parent_.tls_.registerStream(this);
  rw_->StartCall(&init_tag_);
  ++inflight_tags_;
}

void BufAsyncStreamImpl::notifyRemoteClose(Status::ConnectStatus connect_status,
                                              Http::ResponseTrailerMapPtr trailing_metadata,
                                              const std::string& message) {
  if (connect_status > Status::WellKnownConnectStatus::MaximumKnown || connect_status < 0) {
    ENVOY_LOG(error, "notifyRemoteClose invalid Connect status code {}", connect_status);
    // Set the connect_status as InvalidCode but increment the Unknown stream to avoid out-of-range
    // crash..
    connect_status = Status::WellKnownConnectStatus::InvalidCode;
    parent_.stats_.streams_closed_[Status::WellKnownConnectStatus::Unknown]->inc();
  } else {
    parent_.stats_.streams_closed_[connect_status]->inc();
  }
  ENVOY_LOG(debug, "notifyRemoteClose {} {}", connect_status, message);
  callbacks_.onReceiveTrailingMetadata(trailing_metadata ? std::move(trailing_metadata)
                                                         : Http::ResponseTrailerMapImpl::create());
  callbacks_.onRemoteClose(connect_status, message);
}

void BufAsyncStreamImpl::sendMessageRaw(Buffer::InstancePtr&& request, bool end_stream) {
  write_pending_queue_.emplace(std::move(request), end_stream);
  ENVOY_LOG(trace, "Queued message to write ({} bytes)",
            write_pending_queue_.back().buf_.value().Length());
  bytes_in_write_pending_queue_ += write_pending_queue_.back().buf_.value().Length();
  writeQueued();
}

void BufAsyncStreamImpl::closeStream() {
  // Empty EOS write queued.
  write_pending_queue_.emplace();
  writeQueued();
}

void BufAsyncStreamImpl::resetStream() {
  ENVOY_LOG(debug, "resetStream");
  // The Connect API requires calling Finish() at the end of a stream, even
  // if the stream is cancelled.
  if (!finish_pending_) {
    finish_pending_ = true;
    rw_->Finish(&status_, &finish_tag_);
    ++inflight_tags_;
  }
  cleanup();
}

void BufAsyncStreamImpl::writeQueued() {
  if (!call_initialized_ || finish_pending_ || write_pending_ || write_pending_queue_.empty() ||
      draining_cq_) {
    return;
  }
  write_pending_ = true;
  const PendingMessage& msg = write_pending_queue_.front();

  if (!msg.buf_) {
    ASSERT(msg.end_stream_);
    rw_->WritesDone(&write_last_tag_);
    ++inflight_tags_;
  } else if (msg.end_stream_) {
    connect::WriteOptions write_options;
    rw_->WriteLast(msg.buf_.value(), write_options, &write_last_tag_);
    ++inflight_tags_;
  } else {
    rw_->Write(msg.buf_.value(), &write_tag_);
    ++inflight_tags_;
  }
  ENVOY_LOG(trace, "Write op dispatched");
}

void BufAsyncStreamImpl::onCompletedOps() {
  // The items in completed_ops_ execute in the order they were originally added to the queue since
  // both the post callback scheduled by the completionThread and the deferred deletion of the
  // BufAsyncClientThreadLocal happen on the dispatcher thread.
  std::deque<std::pair<BufAsyncTag::Operation, bool>> completed_ops;
  {
    Thread::LockGuard lock(completed_ops_lock_);
    completed_ops = std::move(completed_ops_);
    // completed_ops_ should be empty after the move.
    ASSERT(completed_ops_.empty());
  }

  while (!completed_ops.empty()) {
    BufAsyncTag::Operation op;
    bool ok;
    std::tie(op, ok) = completed_ops.front();
    completed_ops.pop_front();
    handleOpCompletion(op, ok);
  }
}

void BufAsyncStreamImpl::handleOpCompletion(BufAsyncTag::Operation op, bool ok) {
  ENVOY_LOG(trace, "handleOpCompletion op={} ok={} inflight={}", op, ok, inflight_tags_);
  ASSERT(inflight_tags_ > 0);
  --inflight_tags_;
  if (draining_cq_) {
    if (inflight_tags_ == 0) {
      deferredDelete();
    }
    // Ignore op completions while draining CQ.
    return;
  }
  // Consider failure cases first.
  if (!ok) {
    // Early fails can be just treated as Internal.
    if (op == BufAsyncTag::Operation::Init ||
        op == BufAsyncTag::Operation::ReadInitialMetadata) {
      notifyRemoteClose(Status::WellKnownConnectStatus::Internal, nullptr, EMPTY_STRING);
      resetStream();
      return;
    }
    // Remote server has closed, we can pick up some meaningful status.
    // TODO(htuch): We're assuming here that a failed Write/WriteLast operation will result in
    // stream termination, and pick up on the failed Read here. Confirm that this assumption is
    // valid.
    if (op == BufAsyncTag::Operation::Read) {
      finish_pending_ = true;
      rw_->Finish(&status_, &finish_tag_);
      ++inflight_tags_;
    }
    return;
  }
  switch (op) {
  case BufAsyncTag::Operation::Init: {
    ASSERT(ok);
    ASSERT(!call_initialized_);
    call_initialized_ = true;
    rw_->ReadInitialMetadata(&read_initial_metadata_tag_);
    ++inflight_tags_;
    writeQueued();
    break;
  }
  case BufAsyncTag::Operation::ReadInitialMetadata: {
    ASSERT(ok);
    ASSERT(call_initialized_);
    rw_->Read(&read_buf_, &read_tag_);
    ++inflight_tags_;
    Http::ResponseHeaderMapPtr initial_metadata = Http::ResponseHeaderMapImpl::create();
    metadataTranslate(ctxt_.GetServerInitialMetadata(), *initial_metadata);
    callbacks_.onReceiveInitialMetadata(std::move(initial_metadata));
    break;
  }
  case BufAsyncTag::Operation::Write: {
    ASSERT(ok);
    write_pending_ = false;
    bytes_in_write_pending_queue_ -= write_pending_queue_.front().buf_.value().Length();
    write_pending_queue_.pop();
    writeQueued();
    break;
  }
  case BufAsyncTag::Operation::WriteLast: {
    ASSERT(ok);
    write_pending_ = false;
    break;
  }
  case BufAsyncTag::Operation::Read: {
    ASSERT(ok);
    auto buffer = BufConnectUtils::makeBufferInstance(read_buf_);
    if (!buffer || !callbacks_.onReceiveMessageRaw(std::move(buffer))) {
      // This is basically streamError in Connect::AsyncClientImpl.
      notifyRemoteClose(Status::WellKnownConnectStatus::Internal, nullptr, EMPTY_STRING);
      resetStream();
      break;
    }
    rw_->Read(&read_buf_, &read_tag_);
    ++inflight_tags_;
    break;
  }
  case BufAsyncTag::Operation::Finish: {
    ASSERT(finish_pending_);
    ENVOY_LOG(debug, "Finish with connect-status code {}", status_.error_code());
    Http::ResponseTrailerMapPtr trailing_metadata = Http::ResponseTrailerMapImpl::create();
    metadataTranslate(ctxt_.GetServerTrailingMetadata(), *trailing_metadata);
    notifyRemoteClose(static_cast<Status::ConnectStatus>(status_.error_code()),
                      std::move(trailing_metadata), status_.error_message());
    cleanup();
    break;
  }
  }
}

void BufAsyncStreamImpl::metadataTranslate(
    const std::multimap<connect::string_ref, connect::string_ref>& connect_metadata,
    Http::HeaderMap& header_map) {
  // More painful copying, this time due to the mismatch in header
  // representation data structures in Envoy and Buf Connect.
  for (const auto& it : connect_metadata) {
    auto key = Http::LowerCaseString(std::string(it.first.data(), it.first.size()));
    if (absl::EndsWith(key.get(), "-bin")) {
      auto value = Base64::encode(it.second.data(), it.second.size());
      header_map.addCopy(key, value);
      continue;
    }
    header_map.addCopy(key, std::string(it.second.data(), it.second.size()));
  }
}

void BufAsyncStreamImpl::deferredDelete() {
  ENVOY_LOG(debug, "Deferred delete");
  tls_.unregisterStream(this);
  // We only get here following cleanup(), which has performed a
  // remoteFromList(), resulting in self-ownership of the object's memory.
  // Hence, it is safe here to create a unique_ptr to this and transfer
  // ownership to dispatcher_.deferredDelete(). After this call, no further
  // methods may be invoked on this object.
  dispatcher_.deferredDelete(BufAsyncStreamImplPtr(this));
}

void BufAsyncStreamImpl::cleanup() {
  ENVOY_LOG(debug, "Stream cleanup with {} in-flight tags", inflight_tags_);
  // We can get here if the client has already issued resetStream() and, while
  // this is in progress, the destructor runs.
  if (draining_cq_) {
    ENVOY_LOG(debug, "Cleanup already in progress");
    return;
  }
  draining_cq_ = true;
  ctxt_.TryCancel();
  if (LinkedObject<BufAsyncStreamImpl>::inserted()) {
    // We take ownership of our own memory at this point.
    LinkedObject<BufAsyncStreamImpl>::removeFromList(parent_.active_streams_).release();
    if (inflight_tags_ == 0) {
      deferredDelete();
    }
  }
}

BufAsyncRequestImpl::BufAsyncRequestImpl(
    BufAsyncClientImpl& parent, absl::string_view service_full_name,
    absl::string_view method_name, Buffer::InstancePtr request, RawAsyncRequestCallbacks& callbacks,
    Tracing::Span& parent_span, const Http::AsyncClient::RequestOptions& options)
    : BufAsyncStreamImpl(parent, service_full_name, method_name, *this, options),
      request_(std::move(request)), callbacks_(callbacks) {
  current_span_ =
      parent_span.spawnChild(Tracing::EgressConfig::get(),
                             absl::StrCat("async ", service_full_name, ".", method_name, " egress"),
                             parent.timeSource().systemTime());
  current_span_->setTag(Tracing::Tags::get().UpstreamCluster, parent.stat_prefix_);
  current_span_->setTag(Tracing::Tags::get().UpstreamAddress, parent.target_uri_);
  current_span_->setTag(Tracing::Tags::get().Component, Tracing::Tags::get().Proxy);
}

void BufAsyncRequestImpl::initialize(bool buffer_body_for_retry) {
  BufAsyncStreamImpl::initialize(buffer_body_for_retry);
  if (callFailed()) {
    return;
  }
  sendMessageRaw(std::move(request_), true);
}

void BufAsyncRequestImpl::cancel() {
  current_span_->setTag(Tracing::Tags::get().Status, Tracing::Tags::get().Canceled);
  current_span_->finishSpan();
  resetStream();
}

void BufAsyncRequestImpl::onCreateInitialMetadata(Http::RequestHeaderMap& metadata) {
  current_span_->injectContext(metadata, nullptr);
  callbacks_.onCreateInitialMetadata(metadata);
}

void BufAsyncRequestImpl::onReceiveInitialMetadata(Http::ResponseHeaderMapPtr&&) {}

bool BufAsyncRequestImpl::onReceiveMessageRaw(Buffer::InstancePtr&& response) {
  response_ = std::move(response);
  return true;
}

void BufAsyncRequestImpl::onReceiveTrailingMetadata(Http::ResponseTrailerMapPtr&&) {}

void BufAsyncRequestImpl::onRemoteClose(Connect::Status::ConnectStatus status,
                                           const std::string& message) {
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
