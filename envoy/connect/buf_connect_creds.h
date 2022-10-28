#pragma once

#include <memory>

#include "envoy/api/api.h"
#include "envoy/common/pure.h"
#include "envoy/config/core/v3/connect_service.pb.h"
#include "envoy/config/typed_config.h"

#include "connectpp/connectpp.h"

namespace Envoy {
namespace Connect {

/**
 * Interface for all Buf Connect credentials factories.
 */
class BufConnectCredentialsFactory : public Config::UntypedFactory {
public:
  ~BufConnectCredentialsFactory() override = default;

  /**
   * Get a ChannelCredentials to be used for authentication of a Connect channel.
   *
   * BufConnectCredentialsFactory should always return a ChannelCredentials. To use CallCredentials,
   * the ChannelCredentials can be created by using a combination of CompositeChannelCredentials and
   * CompositeCallCredentials to combine multiple credentials.
   *
   * @param connect_service_config contains configuration options
   * @param api reference to the Api object
   * @return std::shared_ptr<connect::ChannelCredentials> to be used to authenticate a Buf Connect
   * channel.
   */
  virtual std::shared_ptr<connect::ChannelCredentials>
  getChannelCredentials(const envoy::config::core::v3::ConnectService& connect_service_config,
                        Api::Api& api) PURE;

  std::string category() const override { return "envoy.connect_credentials"; }
};

} // namespace Connect
} // namespace Envoy
