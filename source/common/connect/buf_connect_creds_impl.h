#pragma once

#include "envoy/api/api.h"
#include "envoy/common/platform.h"
#include "envoy/config/core/v3/connect_service.pb.h"

#include "connectpp/connectpp.h"

namespace Envoy {
namespace Connect {

connect::SslCredentialsOptions buildSslOptionsFromConfig(
    const envoy::config::core::v3::ConnectService::BufConnect::SslCredentials& ssl_config);

std::shared_ptr<connect::ChannelCredentials>
getBufConnectChannelCredentials(const envoy::config::core::v3::ConnectService& connect_service,
                                Api::Api& api);

class CredsUtility {
public:
  /**
   * Translation from envoy::config::core::v3::ConnectService::BufConnect to connect::ChannelCredentials
   * for channel credentials.
   * @param buf_connect Buf Connect config.
   * @param api reference to the Api object
   * @return std::shared_ptr<connect::ChannelCredentials> channel credentials. A nullptr
   *         will be returned in the absence of any configured credentials.
   */
  static std::shared_ptr<connect::ChannelCredentials>
  getChannelCredentials(const envoy::config::core::v3::ConnectService::BufConnect& buf_connect,
                        Api::Api& api);

  /**
   * Static translation from envoy::config::core::v3::ConnectService::BufConnect to a vector of
   * connect::CallCredentials. Any plugin based call credentials will be elided.
   * @param connect_service Buf Connect config.
   * @return std::vector<std::shared_ptr<connect::CallCredentials>> call credentials.
   */
  static std::vector<std::shared_ptr<connect::CallCredentials>>
  callCredentials(const envoy::config::core::v3::ConnectService::BufConnect& buf_connect);

  /**
   * Default translation from envoy::config::core::v3::ConnectService::BufConnect to
   * connect::ChannelCredentials for SSL channel credentials.
   * @param connect_service_config Connect service config.
   * @param api reference to the Api object
   * @return std::shared_ptr<connect::ChannelCredentials> SSL channel credentials. Empty SSL
   *         credentials will be set in the absence of any configured SSL in connect_service_config,
   *         forcing the channel to SSL.
   */
  static std::shared_ptr<connect::ChannelCredentials>
  defaultSslChannelCredentials(const envoy::config::core::v3::ConnectService& connect_service_config,
                               Api::Api& api);

  /**
   * Default static translation from envoy::config::core::v3::ConnectService::BufConnect to
   * connect::ChannelCredentials for all non-plugin based channel and call credentials.
   * @param connect_service_config Connect service config.
   * @param api reference to the Api object
   * @return std::shared_ptr<connect::ChannelCredentials> composite channel and call credentials.
   *         will be set in the absence of any configured SSL in connect_service_config, forcing the
   *         channel to SSL.
   */
  static std::shared_ptr<connect::ChannelCredentials>
  defaultChannelCredentials(const envoy::config::core::v3::ConnectService& connect_service_config,
                            Api::Api& api);
};

} // namespace Connect
} // namespace Envoy
