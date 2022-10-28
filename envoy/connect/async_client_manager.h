#pragma once

#include "envoy/config/core/v3/connect_service.pb.h"
#include "envoy/connect/async_client.h"
#include "envoy/stats/scope.h"

namespace Envoy {
namespace Connect {

class AsyncClientFactoryImpl;

// Per-service factory for Connect::RawAsyncClients. This factory is thread aware and will instantiate
// with thread local state. Clients will use ThreadLocal::Instance::dispatcher() for event handling.
class AsyncClientFactory {
public:
  virtual ~AsyncClientFactory() = default;

  /**
   * Create a Connect::RawAsyncClient.
   * Prefer AsyncClientManager::getOrCreateRawAsyncClient() to creating uncached raw async client
   * from factory directly. Only call this method when the raw async client must be owned
   * exclusively. For example, some filters pass *this reference to raw client. In this case, the
   * client must be destroyed before the filter instance. In this case, the connect client must be
   * owned by the filter instance exclusively.
   * @return RawAsyncClientPtr async client.
   */
  virtual RawAsyncClientPtr createUncachedRawAsyncClient() PURE;

private:
  friend class AsyncClientFactoryImpl;
};

using AsyncClientFactoryPtr = std::unique_ptr<AsyncClientFactory>;

// Singleton Connect client manager. Connect::AsyncClientManager can be used to create per-service
// Connect::AsyncClientFactory instances. All manufactured Connect::AsyncClients must
// be destroyed before the AsyncClientManager can be safely destructed.
class AsyncClientManager {
public:
  virtual ~AsyncClientManager() = default;

  /**
   * Create a Connect::RawAsyncClient. The async client is cached thread locally and shared across
   * different filter instances.
   * @param connect_service envoy::config::core::v3::ConnectService configuration.
   * @param scope stats scope.
   * @param skip_cluster_check if set to true skips checks for cluster presence and being statically
   * configured.
   * @param cache_option always use cache or use cache when runtime is enabled.
   * @return RawAsyncClientPtr a connect async client.
   * @throws EnvoyException when connect_service validation fails.
   */
  virtual RawAsyncClientSharedPtr
  getOrCreateRawAsyncClient(const envoy::config::core::v3::ConnectService& connect_service,
                            Stats::Scope& scope, bool skip_cluster_check) PURE;

  /**
   * Create a Connect::AsyncClients factory for a service. Validation of the service is performed and
   * will raise an exception on failure.
   * @param connect_service envoy::config::core::v3::ConnectService configuration.
   * @param scope stats scope.
   * @param skip_cluster_check if set to true skips checks for cluster presence and being statically
   * configured.
   * @return AsyncClientFactoryPtr factory for connect_service.
   * @throws EnvoyException when connect_service validation fails.
   */
  virtual AsyncClientFactoryPtr
  factoryForConnectService(const envoy::config::core::v3::ConnectService& connect_service,
                        Stats::Scope& scope, bool skip_cluster_check) PURE;
};

using AsyncClientManagerPtr = std::unique_ptr<AsyncClientManager>;

} // namespace Connect
} // namespace Envoy
