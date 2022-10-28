#include "source/common/connect/async_client_manager_impl.h"

#include "envoy/config/core/v3/connect_service.pb.h"
#include "envoy/stats/scope.h"

#include "source/common/common/base64.h"
#include "source/common/connect/async_client_impl.h"

#include "absl/strings/match.h"

#ifdef ENVOY_BUF_CONNECT
#include "source/common/connect/buf_async_client_impl.h"
#endif

namespace Envoy {
namespace Connect {
namespace {

// Validates a string for Connect header key compliance. This is a subset of legal HTTP characters.
// See https://github.com/connect/connect/blob/master/doc/PROTOCOL-HTTP2.md
bool validateConnectHeaderChars(absl::string_view key) {
  for (auto ch : key) {
    if (!(absl::ascii_isalnum(ch) || ch == '_' || ch == '.' || ch == '-')) {
      return false;
    }
  }
  return true;
}

bool validateConnectCompatibleAsciiHeaderValue(absl::string_view h_value) {
  for (auto ch : h_value) {
    if (ch < 0x20 || ch > 0x7e) {
      return false;
    }
  }
  return true;
}

} // namespace

AsyncClientFactoryImpl::AsyncClientFactoryImpl(Upstream::ClusterManager& cm,
                                               const envoy::config::core::v3::ConnectService& config,
                                               bool skip_cluster_check, TimeSource& time_source)
    : cm_(cm), config_(config), time_source_(time_source) {
  if (skip_cluster_check) {
    return;
  }
  cm_.checkActiveStaticCluster(config.envoy_connect().cluster_name());
}

AsyncClientManagerImpl::AsyncClientManagerImpl(Upstream::ClusterManager& cm,
                                               ThreadLocal::Instance& tls, TimeSource& time_source,
                                               Api::Api& api, const StatNames& stat_names)
    : cm_(cm), tls_(tls), time_source_(time_source), api_(api), stat_names_(stat_names),
      raw_async_client_cache_(tls_) {
  raw_async_client_cache_.set([](Event::Dispatcher& dispatcher) {
    return std::make_shared<RawAsyncClientCache>(dispatcher);
  });
#ifdef ENVOY_BUF_CONNECT
  buf_tls_slot_ = tls.allocateSlot();
  buf_tls_slot_->set(
      [&api](Event::Dispatcher&) { return std::make_shared<BufAsyncClientThreadLocal>(api); });
#else
  UNREFERENCED_PARAMETER(api_);
#endif
}

RawAsyncClientPtr AsyncClientFactoryImpl::createUncachedRawAsyncClient() {
  return std::make_unique<AsyncClientImpl>(cm_, config_, time_source_);
}

BufAsyncClientFactoryImpl::BufAsyncClientFactoryImpl(
    ThreadLocal::Instance& tls, ThreadLocal::Slot* buf_tls_slot, Stats::Scope& scope,
    const envoy::config::core::v3::ConnectService& config, Api::Api& api, const StatNames& stat_names)
    : tls_(tls), buf_tls_slot_(buf_tls_slot),
      scope_(scope.createScope(fmt::format("connect.{}.", config.buf_connect().stat_prefix()))),
      config_(config), api_(api), stat_names_(stat_names) {

#ifndef ENVOY_BUF_CONNECT
  UNREFERENCED_PARAMETER(tls_);
  UNREFERENCED_PARAMETER(buf_tls_slot_);
  UNREFERENCED_PARAMETER(scope_);
  UNREFERENCED_PARAMETER(config_);
  UNREFERENCED_PARAMETER(api_);
  UNREFERENCED_PARAMETER(stat_names_);
  throw EnvoyException("Buf C++ Connect client is not linked");
#else
  ASSERT(buf_tls_slot_ != nullptr);
#endif

  // Check metadata for Connect API compliance. Uppercase characters are lowered in the HeaderParser.
  // https://github.com/connect/connect/blob/master/doc/PROTOCOL-HTTP2.md
  for (const auto& header : config.initial_metadata()) {
    // Validate key
    if (!validateConnectHeaderChars(header.key())) {
      throw EnvoyException(
          fmt::format("Illegal characters in Connect initial metadata header key: {}.", header.key()));
    }

    // Validate value
    // Binary base64 encoded - handled by the CONNECT library
    if (!::absl::EndsWith(header.key(), "-bin") &&
        !validateConnectCompatibleAsciiHeaderValue(header.value())) {
      throw EnvoyException(fmt::format(
          "Illegal ASCII value for Connect initial metadata header key: {}.", header.key()));
    }
  }
}

RawAsyncClientPtr BufAsyncClientFactoryImpl::createUncachedRawAsyncClient() {
#ifdef ENVOY_BUF_CONNECT
  BufGenericStubFactory stub_factory;
  return std::make_unique<BufAsyncClientImpl>(
      tls_.dispatcher(), buf_tls_slot_->getTyped<BufAsyncClientThreadLocal>(), stub_factory,
      scope_, config_, api_, stat_names_);
#else
  return nullptr;
#endif
}

AsyncClientFactoryPtr
AsyncClientManagerImpl::factoryForConnectService(const envoy::config::core::v3::ConnectService& config,
                                              Stats::Scope& scope, bool skip_cluster_check) {
  switch (config.target_specifier_case()) {
  case envoy::config::core::v3::ConnectService::TargetSpecifierCase::kEnvoyConnect:
    return std::make_unique<AsyncClientFactoryImpl>(cm_, config, skip_cluster_check, time_source_);
  case envoy::config::core::v3::ConnectService::TargetSpecifierCase::kBufConnect:
    return std::make_unique<BufAsyncClientFactoryImpl>(tls_, buf_tls_slot_.get(), scope,
                                                          config, api_, stat_names_);
  case envoy::config::core::v3::ConnectService::TargetSpecifierCase::TARGET_SPECIFIER_NOT_SET:
    PANIC_DUE_TO_PROTO_UNSET;
  }
  return nullptr;
}

RawAsyncClientSharedPtr AsyncClientManagerImpl::getOrCreateRawAsyncClient(
    const envoy::config::core::v3::ConnectService& config, Stats::Scope& scope,
    bool skip_cluster_check) {
  RawAsyncClientSharedPtr client = raw_async_client_cache_->getCache(config);
  if (client != nullptr) {
    return client;
  }
  client = factoryForConnectService(config, scope, skip_cluster_check)->createUncachedRawAsyncClient();
  raw_async_client_cache_->setCache(config, client);
  return client;
}

AsyncClientManagerImpl::RawAsyncClientCache::RawAsyncClientCache(Event::Dispatcher& dispatcher)
    : dispatcher_(dispatcher) {
  cache_eviction_timer_ = dispatcher.createTimer([this] { evictEntriesAndResetEvictionTimer(); });
}

void AsyncClientManagerImpl::RawAsyncClientCache::setCache(
    const envoy::config::core::v3::ConnectService& config, const RawAsyncClientSharedPtr& client) {
  ASSERT(lru_map_.find(config) == lru_map_.end());
  // Create a new cache entry at the beginning of the list.
  lru_list_.emplace_front(config, client, dispatcher_.timeSource().monotonicTime());
  lru_map_[config] = lru_list_.begin();
  // If inserting to an empty cache, enable eviction timer.
  if (lru_list_.size() == 1) {
    evictEntriesAndResetEvictionTimer();
  }
}

RawAsyncClientSharedPtr AsyncClientManagerImpl::RawAsyncClientCache::getCache(
    const envoy::config::core::v3::ConnectService& config) {
  auto it = lru_map_.find(config);
  if (it == lru_map_.end()) {
    return nullptr;
  }
  const auto cache_entry = it->second;
  // Reset the eviction timer if the next entry to expire is accessed.
  const bool should_reset_timer = (cache_entry == --lru_list_.end());
  cache_entry->accessed_time_ = dispatcher_.timeSource().monotonicTime();
  // Move the cache entry to the beginning of the list upon access.
  lru_list_.splice(lru_list_.begin(), lru_list_, cache_entry);
  // Get the cached async client before any cache eviction.
  RawAsyncClientSharedPtr client = cache_entry->client_;
  if (should_reset_timer) {
    evictEntriesAndResetEvictionTimer();
  }
  return client;
}

void AsyncClientManagerImpl::RawAsyncClientCache::evictEntriesAndResetEvictionTimer() {
  MonotonicTime now = dispatcher_.timeSource().monotonicTime();
  // Evict all the entries that have expired.
  while (!lru_list_.empty()) {
    MonotonicTime next_expire = lru_list_.back().accessed_time_ + EntryTimeoutInterval;
    if (now >= next_expire) {
      // Erase the expired entry.
      lru_map_.erase(lru_list_.back().config_);
      lru_list_.pop_back();
    } else {
      cache_eviction_timer_->enableTimer(
          std::chrono::duration_cast<std::chrono::seconds>(next_expire - now));
      return;
    }
  }
}

} // namespace Connect
} // namespace Envoy
