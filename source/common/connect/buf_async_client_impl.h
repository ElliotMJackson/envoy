#pragma once

#include <memory>
#include <queue>

#include "envoy/api/api.h"
#include "envoy/common/platform.h"
#include "envoy/config/core/v3/base.pb.h"
#include "envoy/config/core/v3/connect_service.pb.h"
#include "envoy/connect/async_client.h"
#include "envoy/stats/scope.h"
#include "envoy/thread/thread.h"
#include "envoy/thread_local/thread_local_object.h"
#include "envoy/tracing/http_tracer.h"

#include "source/common/common/linked_object.h"
#include "source/common/common/thread.h"
#include "source/common/common/thread_annotations.h"
#include "source/common/connect/buf_connect_context.h"
#include "source/common/connect/stat_names.h"
#include "source/common/connect/typed_async_client.h"
#include "source/common/router/header_parser.h"
#include "source/common/tracing/http_tracer_impl.h"

#include "absl/container/node_hash_set.h"
#include "connectpp/generic/generic_stub.h"
#include "connectpp/connectpp.h"
#include "connectpp/support/proto_buffer_writer.h"

namespace Envoy {
namespace Connect {

class BufAsyncStreamImpl;

using BufAsyncStreamImplPtr = std::unique_ptr<BufAsyncStreamImpl>;

class BufAsyncRequestImpl;

struct BufAsyncTag {
  // Operation defines tags that are handed to the Connect AsyncReaderWriter for use in completion
  // notification for their namesake operations. Read* and Write* operations may be outstanding
  // simultaneously, but there will be no more than one operation of each type in-flight for a given
  // stream. Init and Finish will both be issued exclusively when no other operations are in-flight
  // for a stream. See
  // https://github.com/connect/connect/blob/master/include/connect%2B%2B/impl/codegen/async_stream.h for
  // further insight into the semantics of the different Connect client operations.
  enum Operation {
    // Initial stub call issued, waiting for initialization to complete.
    Init = 0,
    // Waiting for initial meta-data from server following Init completion.
    ReadInitialMetadata,
    // Waiting for response protobuf from server following ReadInitialMetadata completion.
    Read,
    // Waiting for write of request protobuf to server to complete.
    Write,
    // Waiting for write of request protobuf (EOS) __OR__ an EOS WritesDone to server to complete.
    WriteLast,
    // Waiting for final status. This must only be issued once all Read* and Write* operations have
    // completed.
    Finish,
  };

  BufAsyncTag(BufAsyncStreamImpl& stream, Operation op) : stream_(stream), op_(op) {}

  BufAsyncStreamImpl& stream_;
  const Operation op_;
};

class BufAsyncClientThreadLocal : public ThreadLocal::ThreadLocalObject,
                                     Logger::Loggable<Logger::Id::connect> {
public:
  BufAsyncClientThreadLocal(Api::Api& api);
  ~BufAsyncClientThreadLocal() override;

  connect::CompletionQueue& completionQueue() { return cq_; }

  void registerStream(BufAsyncStreamImpl* stream) {
    ASSERT(streams_.find(stream) == streams_.end());
    streams_.insert(stream);
  }

  void unregisterStream(BufAsyncStreamImpl* stream) {
    auto it = streams_.find(stream);
    ASSERT(it != streams_.end());
    streams_.erase(it);
  }

private:
  void completionThread();

  // There is blanket buf-connect initialization in MainCommonBase, but that
  // doesn't cover unit tests. However, putting blanket coverage in ProcessWide
  // causes background threaded memory allocation in all unit tests making it
  // hard to measure memory. Thus we also initialize connect using our idempotent
  // wrapper-class in classes that need it. See
  // https://github.com/envoyproxy/envoy/issues/8282 for details.
  BufConnectContext buf_connect_context_;

  // The CompletionQueue for in-flight operations. This must precede completion_thread_ to ensure it
  // is constructed before the thread runs.
  connect::CompletionQueue cq_;
  // The threading model for the Buf Connect C++ library is not directly compatible with Envoy's
  // siloed model. We resolve this by issuing non-blocking asynchronous
  // operations on the BufAsyncClientImpl silo thread, and then synchronously
  // blocking on a completion queue, cq_, on a distinct thread. When cq_ events
  // are delivered, we cross-post to the silo dispatcher to continue the
  // operation.
  //
  // We have an independent completion thread for each TLS silo (i.e. one per worker and
  // also one for the main thread).
  Thread::ThreadPtr completion_thread_;
  // Track all streams that are currently using this CQ, so we can notify them
  // on shutdown.
  absl::node_hash_set<BufAsyncStreamImpl*> streams_;
};

using BufAsyncClientThreadLocalPtr = std::unique_ptr<BufAsyncClientThreadLocal>;

// Buf Connect client stats. TODO(htuch): consider how a wider set of stats collected by the
// library, such as the census related ones, can be externalized as needed.
struct BufAsyncClientStats {
  // .streams_total
  Stats::Counter* streams_total_;
  // .streams_closed_<Connect status code>
  std::array<Stats::Counter*, Status::WellKnownConnectStatus::MaximumKnown + 1> streams_closed_;
};

// Interface to allow the Connect stub to be mocked out by tests.
class BufStub {
public:
  virtual ~BufStub() = default;

  // See connect::PrepareCall().
  virtual std::unique_ptr<connect::GenericClientAsyncReaderWriter>
  PrepareCall(connect::ClientContext* context, const connect::string& method,
              connect::CompletionQueue* cq) PURE;
};

using BufStubSharedPtr = std::shared_ptr<BufStub>;

class BufGenericStub : public BufStub {
public:
  BufGenericStub(std::shared_ptr<connect::Channel> channel) : stub_(channel) {}

  std::unique_ptr<connect::GenericClientAsyncReaderWriter>
  PrepareCall(connect::ClientContext* context, const connect::string& method,
              connect::CompletionQueue* cq) override {
    return stub_.PrepareCall(context, method, cq);
  }

private:
  connect::GenericStub stub_;
};

// Interface to allow the Connect stub creation to be mocked out by tests.
class BufStubFactory {
public:
  virtual ~BufStubFactory() = default;

  // Create a stub from a given channel.
  virtual BufStubSharedPtr createStub(std::shared_ptr<connect::Channel> channel) PURE;
};

class BufGenericStubFactory : public BufStubFactory {
public:
  BufStubSharedPtr createStub(std::shared_ptr<connect::Channel> channel) override {
    return std::make_shared<BufGenericStub>(channel);
  }
};

// Buf Connect C++ client library implementation of Connect::AsyncClient.
class BufAsyncClientImpl final : public RawAsyncClient, Logger::Loggable<Logger::Id::connect> {
public:
  BufAsyncClientImpl(Event::Dispatcher& dispatcher, BufAsyncClientThreadLocal& tls,
                        BufStubFactory& stub_factory, Stats::ScopeSharedPtr scope,
                        const envoy::config::core::v3::ConnectService& config, Api::Api& api,
                        const StatNames& stat_names);
  ~BufAsyncClientImpl() override;

  // Connect::AsyncClient
  AsyncRequest* sendRaw(absl::string_view service_full_name, absl::string_view method_name,
                        Buffer::InstancePtr&& request, RawAsyncRequestCallbacks& callbacks,
                        Tracing::Span& parent_span,
                        const Http::AsyncClient::RequestOptions& options) override;
  RawAsyncStream* startRaw(absl::string_view service_full_name, absl::string_view method_name,
                           RawAsyncStreamCallbacks& callbacks,
                           const Http::AsyncClient::StreamOptions& options) override;
  absl::string_view destination() override { return target_uri_; }

  TimeSource& timeSource() { return dispatcher_.timeSource(); }
  uint64_t perStreamBufferLimitBytes() const { return per_stream_buffer_limit_bytes_; }

private:
  Event::Dispatcher& dispatcher_;
  BufAsyncClientThreadLocal& tls_;
  // This is shared with child streams, so that they can cleanup independent of
  // the client if it gets destructed. The streams need to wait for their tags
  // to drain from the CQ.
  BufStubSharedPtr stub_;
  std::list<BufAsyncStreamImplPtr> active_streams_;
  const std::string stat_prefix_;
  const std::string target_uri_;
  Stats::ScopeSharedPtr scope_;
  BufAsyncClientStats stats_;
  uint64_t per_stream_buffer_limit_bytes_;
  Router::HeaderParserPtr metadata_parser_;

  friend class BufAsyncClientThreadLocal;
  friend class BufAsyncRequestImpl;
  friend class BufAsyncStreamImpl;
};

class BufAsyncStreamImpl : public RawAsyncStream,
                              public Event::DeferredDeletable,
                              Logger::Loggable<Logger::Id::connect>,
                              public LinkedObject<BufAsyncStreamImpl> {
public:
  BufAsyncStreamImpl(BufAsyncClientImpl& parent, absl::string_view service_full_name,
                        absl::string_view method_name, RawAsyncStreamCallbacks& callbacks,
                        const Http::AsyncClient::StreamOptions& options);
  ~BufAsyncStreamImpl() override;

  virtual void initialize(bool buffer_body_for_retry);

  // Connect::RawAsyncStream
  void sendMessageRaw(Buffer::InstancePtr&& request, bool end_stream) override;
  void closeStream() override;
  void resetStream() override;
  // While the Buf-Connect code doesn't use Envoy watermark buffers, the logical
  // analog is to make sure that the aren't too many bytes in the pending write
  // queue.
  bool isAboveWriteBufferHighWatermark() const override {
    return bytes_in_write_pending_queue_ > parent_.perStreamBufferLimitBytes();
  }

protected:
  bool callFailed() const { return call_failed_; }

private:
  // Process queued events in completed_ops_ with handleOpCompletion() on
  // BufAsyncClient silo thread.
  void onCompletedOps();
  // Handle Operation completion on BufAsyncClient silo thread. This is posted by
  // BufAsyncClientThreadLocal::completionThread() when a message is received on cq_.
  void handleOpCompletion(BufAsyncTag::Operation op, bool ok);
  // Convert from Buf Connect client std::multimap metadata to Envoy Http::HeaderMap.
  void metadataTranslate(const std::multimap<connect::string_ref, connect::string_ref>& connect_metadata,
                         Http::HeaderMap& header_map);
  // Write the first PendingMessage in the write queue if non-empty.
  void writeQueued();
  // Deliver notification and update stats when the connection closes.
  void notifyRemoteClose(Status::ConnectStatus connect_status,
                         Http::ResponseTrailerMapPtr trailing_metadata, const std::string& message);
  // Schedule stream for deferred deletion.
  void deferredDelete();
  // Cleanup and schedule stream for deferred deletion if no inflight
  // completions.
  void cleanup();

  // Pending serialized message on write queue. Only one Operation::Write is in-flight at any
  // point-in-time, so we queue pending writes here.
  struct PendingMessage {
    PendingMessage(Buffer::InstancePtr request, bool end_stream);
    // End-of-stream with no additional message.
    PendingMessage() = default;

    const absl::optional<connect::ByteBuffer> buf_{};
    const bool end_stream_{true};
  };

  BufAsyncTag init_tag_{*this, BufAsyncTag::Operation::Init};
  BufAsyncTag read_initial_metadata_tag_{*this, BufAsyncTag::Operation::ReadInitialMetadata};
  BufAsyncTag read_tag_{*this, BufAsyncTag::Operation::Read};
  BufAsyncTag write_tag_{*this, BufAsyncTag::Operation::Write};
  BufAsyncTag write_last_tag_{*this, BufAsyncTag::Operation::WriteLast};
  BufAsyncTag finish_tag_{*this, BufAsyncTag::Operation::Finish};

  BufAsyncClientImpl& parent_;
  BufAsyncClientThreadLocal& tls_;
  // Latch our own version of this reference, so that completionThread() doesn't
  // try and access via parent_, which might not exist in teardown. We assume
  // that the dispatcher lives longer than completionThread() life, which should
  // hold for the expected server object lifetimes.
  Event::Dispatcher& dispatcher_;
  // We hold a ref count on the stub_ to allow the stream to wait for its tags
  // to drain from the CQ on cleanup.
  BufStubSharedPtr stub_;
  std::string service_full_name_;
  std::string method_name_;
  RawAsyncStreamCallbacks& callbacks_;
  const Http::AsyncClient::StreamOptions& options_;
  connect::ClientContext ctxt_;
  std::unique_ptr<connect::GenericClientAsyncReaderWriter> rw_;
  std::queue<PendingMessage> write_pending_queue_;
  uint64_t bytes_in_write_pending_queue_{};
  connect::ByteBuffer read_buf_;
  connect::Status status_;
  // Has Operation::Init completed?
  bool call_initialized_{};
  // Did the stub Call fail? If this is true, no Operation::Init completion will ever occur.
  bool call_failed_{};
  // Is there an Operation::Write[Last] in-flight?
  bool write_pending_{};
  // Is an Operation::Finish in-flight?
  bool finish_pending_{};
  // Have we entered CQ draining state? If so, we're just waiting for all our
  // ops on the CQ to drain away before freeing the stream.
  bool draining_cq_{};
  // Count of the tags in-flight. This must hit zero before the stream can be
  // freed.
  uint32_t inflight_tags_{};
  // Queue of completed (op, ok) passed from completionThread() to
  // handleOpCompletion().
  std::deque<std::pair<BufAsyncTag::Operation, bool>>
      completed_ops_ ABSL_GUARDED_BY(completed_ops_lock_);
  Thread::MutexBasicLockable completed_ops_lock_;

  friend class BufAsyncClientImpl;
  friend class BufAsyncClientThreadLocal;
};

class BufAsyncRequestImpl : public AsyncRequest,
                               public BufAsyncStreamImpl,
                               RawAsyncStreamCallbacks {
public:
  BufAsyncRequestImpl(BufAsyncClientImpl& parent, absl::string_view service_full_name,
                         absl::string_view method_name, Buffer::InstancePtr request,
                         RawAsyncRequestCallbacks& callbacks, Tracing::Span& parent_span,
                         const Http::AsyncClient::RequestOptions& options);

  void initialize(bool buffer_body_for_retry) override;

  // Connect::AsyncRequest
  void cancel() override;

private:
  // Connect::RawAsyncStreamCallbacks
  void onCreateInitialMetadata(Http::RequestHeaderMap& metadata) override;
  void onReceiveInitialMetadata(Http::ResponseHeaderMapPtr&&) override;
  bool onReceiveMessageRaw(Buffer::InstancePtr&& response) override;
  void onReceiveTrailingMetadata(Http::ResponseTrailerMapPtr&&) override;
  void onRemoteClose(Connect::Status::ConnectStatus status, const std::string& message) override;

  Buffer::InstancePtr request_;
  RawAsyncRequestCallbacks& callbacks_;
  Tracing::SpanPtr current_span_;
  Buffer::InstancePtr response_;
};

} // namespace Connect
} // namespace Envoy
