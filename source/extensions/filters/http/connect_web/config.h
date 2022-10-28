#pragma once

#include "envoy/extensions/filters/http/connect_web/v3/connect_web.pb.h"
#include "envoy/extensions/filters/http/connect_web/v3/connect_web.pb.validate.h"

#include "source/extensions/filters/http/common/factory_base.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace ConnectWeb {

class ConnectWebFilterConfig
    : public Common::FactoryBase<envoy::extensions::filters::http::connect_web::v3::ConnectWeb> {
public:
  ConnectWebFilterConfig() : FactoryBase("envoy.filters.http.connect_web") {}

private:
  Http::FilterFactoryCb createFilterFactoryFromProtoTyped(
      const envoy::extensions::filters::http::connect_web::v3::ConnectWeb& proto_config,
      const std::string& stats_prefix,
      Server::Configuration::FactoryContext& factory_context) override;
};

} // namespace ConnectWeb
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
