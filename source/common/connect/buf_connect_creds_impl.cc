#include "source/common/connect/buf_connect_creds_impl.h"

#include "envoy/config/core/v3/connect_service.pb.h"
#include "envoy/connect/buf_connect_creds.h"
#include "envoy/registry/registry.h"

#include "source/common/config/datasource.h"

namespace Envoy {
namespace Connect {

std::shared_ptr<connect::ChannelCredentials> CredsUtility::getChannelCredentials(
    const envoy::config::core::v3::ConnectService::BufConnect& buf_connect, Api::Api& api) {
  if (buf_connect.has_channel_credentials()) {
    switch (buf_connect.channel_credentials().credential_specifier_case()) {
    case envoy::config::core::v3::ConnectService::BufConnect::ChannelCredentials::
        CredentialSpecifierCase::kSslCredentials: {
      const auto& ssl_credentials = buf_connect.channel_credentials().ssl_credentials();
      const connect::SslCredentialsOptions ssl_credentials_options = {
          Config::DataSource::read(ssl_credentials.root_certs(), true, api),
          Config::DataSource::read(ssl_credentials.private_key(), true, api),
          Config::DataSource::read(ssl_credentials.cert_chain(), true, api),
      };
      return connect::SslCredentials(ssl_credentials_options);
    }
    case envoy::config::core::v3::ConnectService::BufConnect::ChannelCredentials::
        CredentialSpecifierCase::kLocalCredentials: {
      return connect::experimental::LocalCredentials(UDS);
    }
    case envoy::config::core::v3::ConnectService::BufConnect::ChannelCredentials::
        CredentialSpecifierCase::kBufDefault: {
      return connect::BufDefaultCredentials();
    }
    default:
      return nullptr;
    }
  }
  return nullptr;
}

std::shared_ptr<connect::ChannelCredentials> CredsUtility::defaultSslChannelCredentials(
    const envoy::config::core::v3::ConnectService& connect_service_config, Api::Api& api) {
  auto creds = getChannelCredentials(connect_service_config.buf_connect(), api);
  if (creds != nullptr) {
    return creds;
  }
  return connect::SslCredentials({});
}

std::vector<std::shared_ptr<connect::CallCredentials>>
CredsUtility::callCredentials(const envoy::config::core::v3::ConnectService::BufConnect& buf_connect) {
  std::vector<std::shared_ptr<connect::CallCredentials>> creds;
  for (const auto& credential : buf_connect.call_credentials()) {
    std::shared_ptr<connect::CallCredentials> new_call_creds;
    switch (credential.credential_specifier_case()) {
    case envoy::config::core::v3::ConnectService::BufConnect::CallCredentials::
        CredentialSpecifierCase::kAccessToken: {
      new_call_creds = connect::AccessTokenCredentials(credential.access_token());
      break;
    }
    case envoy::config::core::v3::ConnectService::BufConnect::CallCredentials::
        CredentialSpecifierCase::kBufComputeEngine: {
      new_call_creds = connect::BufComputeEngineCredentials();
      break;
    }
    case envoy::config::core::v3::ConnectService::BufConnect::CallCredentials::
        CredentialSpecifierCase::kBufRefreshToken: {
      new_call_creds = connect::BufRefreshTokenCredentials(credential.buf_refresh_token());
      break;
    }
    case envoy::config::core::v3::ConnectService::BufConnect::CallCredentials::
        CredentialSpecifierCase::kServiceAccountJwtAccess: {
      new_call_creds = connect::ServiceAccountJWTAccessCredentials(
          credential.service_account_jwt_access().json_key(),
          credential.service_account_jwt_access().token_lifetime_seconds());
      break;
    }
    case envoy::config::core::v3::ConnectService::BufConnect::CallCredentials::
        CredentialSpecifierCase::kBufIam: {
      new_call_creds = connect::BufIAMCredentials(credential.buf_iam().authorization_token(),
                                                  credential.buf_iam().authority_selector());
      break;
    }
    case envoy::config::core::v3::ConnectService::BufConnect::CallCredentials::
        CredentialSpecifierCase::kStsService: {
      connect::experimental::StsCredentialsOptions options = {
          credential.sts_service().token_exchange_service_uri(),
          credential.sts_service().resource(),
          credential.sts_service().audience(),
          credential.sts_service().scope(),
          credential.sts_service().requested_token_type(),
          credential.sts_service().subject_token_path(),
          credential.sts_service().subject_token_type(),
          credential.sts_service().actor_token_path(),
          credential.sts_service().actor_token_type(),
      };
      new_call_creds = connect::experimental::StsCredentials(options);
      break;
    }
    default:
      // We don't handle plugin credentials here, callers can do so instead if they want.
      continue;
    }
    // Any of the above creds creation can fail, if they do they return nullptr
    // and we ignore them.
    if (new_call_creds != nullptr) {
      creds.emplace_back(new_call_creds);
    }
  }
  return creds;
}

std::shared_ptr<connect::ChannelCredentials> CredsUtility::defaultChannelCredentials(
    const envoy::config::core::v3::ConnectService& connect_service_config, Api::Api& api) {
  std::shared_ptr<connect::ChannelCredentials> channel_creds =
      getChannelCredentials(connect_service_config.buf_connect(), api);
  if (channel_creds == nullptr) {
    channel_creds = connect::InsecureChannelCredentials();
  }
  auto call_creds_vec = callCredentials(connect_service_config.buf_connect());
  if (call_creds_vec.empty()) {
    return channel_creds;
  }
  std::shared_ptr<connect::CallCredentials> call_creds = call_creds_vec[0];
  for (uint32_t i = 1; i < call_creds_vec.size(); ++i) {
    call_creds = connect::CompositeCallCredentials(call_creds, call_creds_vec[i]);
  }
  return connect::CompositeChannelCredentials(channel_creds, call_creds);
}

/**
 * Default implementation of Buf Connect Credentials Factory
 * Uses ssl creds if available, or defaults to insecure channel.
 *
 * This is not the same as buf_default credentials. This is the default implementation that is
 * loaded if no other implementation is configured.
 */
class DefaultBufConnectCredentialsFactory : public BufConnectCredentialsFactory {

public:
  std::shared_ptr<connect::ChannelCredentials>
  getChannelCredentials(const envoy::config::core::v3::ConnectService& connect_service_config,
                        Api::Api& api) override {
    return CredsUtility::defaultChannelCredentials(connect_service_config, api);
  }

  std::string name() const override { return "envoy.connect_credentials.default"; }
};

/**
 * Static registration for the default Buf Connect credentials factory. @see RegisterFactory.
 */
REGISTER_FACTORY(DefaultBufConnectCredentialsFactory, BufConnectCredentialsFactory);

} // namespace Connect
} // namespace Envoy
