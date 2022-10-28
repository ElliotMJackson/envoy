#pragma once

#include "envoy/api/api.h"
#include "envoy/config/core/v3/connect_service.pb.h"
#include "envoy/connect/async_client_manager.h"
#include "envoy/singleton/manager.h"
#include "envoy/stats/scope.h"
#include "envoy/thread_local/thread_local.h"
#include "envoy/upstream/cluster_manager.h"

#include "source/common/connect/stat_names.h"

namespace Envoy {
namespace Connect {

class AsyncClientFactoryImpl : public AsyncClientFactory {
public:
  AsyncClientFactoryImpl(Upstream::ClusterManager& cm,
                         const envoy::config::core::v3::ConnectService& config,
                         bool skip_cluster_check, TimeSource& time_source);
  RawAsyncClientPtr createUncachedRawAsyncClient() override;

private:
  Upstream::ClusterManager& cm_;
  const envoy::config::core::v3::ConnectService config_;
  TimeSource& time_source_;
};

class BufAsyncClientFactoryImpl : public AsyncClientFactory {
public:
  BufAsyncClientFactoryImpl(ThreadLocal::Instance& tls, ThreadLocal::Slot* buf_tls_slot,
                               Stats::Scope& scope,
                               const envoy::config::core::v3::ConnectService& config, Api::Api& api,
                               const StatNames& stat_names);
  RawAsyncClientPtr createUncachedRawAsyncClient() override;

private:
  ThreadLocal::Instance& tls_;
  ThreadLocal::Slot* buf_tls_slot_;
  Stats::ScopeSharedPtr scope_;
  const envoy::config::core::v3::ConnectService config_;
  Api::Api& api_;
  const StatNames& stat_names_;
};

class AsyncClientManagerImpl : public AsyncClientManager {
public:
  AsyncClientManagerImpl(Upstream::ClusterManager& cm, ThreadLocal::Instance& tls,
                         TimeSource& time_source, Api::Api& api, const StatNames& stat_names);
  RawAsyncClientSharedPtr
  getOrCreateRawAsyncClient(const envoy::config::core::v3::ConnectService& config, Stats::Scope& scope,
                            bool skip_cluster_check) override;

  AsyncClientFactoryPtr factoryForConnectService(const envoy::config::core::v3::ConnectService& config,
                                              Stats::Scope& scope,
                                              bool skip_cluster_check) override;
  class RawAsyncClientCache : public ThreadLocal::ThreadLocalObject {
  public:
    explicit RawAsyncClientCache(Event::Dispatcher& dispatcher);
    void setCache(const envoy::config::core::v3::ConnectService& config,
                  const RawAsyncClientSharedPtr& client);

    RawAsyncClientSharedPtr getCache(const envoy::config::core::v3::ConnectService& config);

  private:
    void evictEntriesAndResetEvictionTimer();
    struct CacheEntry {
      CacheEntry(const envoy::config::core::v3::ConnectService& config,
                 RawAsyncClientSharedPtr const& client, MonotonicTime create_time)
          : config_(config), client_(client), accessed_time_(create_time) {}
      envoy::config::core::v3::ConnectService config_;
      RawAsyncClientSharedPtr client_;
      MonotonicTime accessed_time_;
    };
    using LruList = std::list<CacheEntry>;
    absl::flat_hash_map<envoy::config::core::v3::ConnectService, LruList::iterator, MessageUtil,
                        MessageUtil>
        lru_map_;
    LruList lru_list_;
    Event::Dispatcher& dispatcher_;
    Envoy::Event::TimerPtr cache_eviction_timer_;
    static constexpr std::chrono::seconds EntryTimeoutInterval{50};
  };

private:
  Upstream::ClusterManager& cm_;
  ThreadLocal::Instance& tls_;
  ThreadLocal::SlotPtr buf_tls_slot_;
  TimeSource& time_source_;
  Api::Api& api_;
  const StatNames& stat_names_;
  ThreadLocal::TypedSlot<RawAsyncClientCache> raw_async_client_cache_;
};

} // namespace Connect
} // namespace Envoy
