#include "source/extensions/filters/http/connect_web/config.h"

#include "envoy/registry/registry.h"

#include "source/extensions/filters/http/connect_web/connect_web_filter.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace ConnectWeb {

Http::FilterFactoryCb ConnectWebFilterConfig::createFilterFactoryFromProtoTyped(
    const envoy::extensions::filters::http::connect_web::v3::ConnectWeb&, const std::string&,
    Server::Configuration::FactoryContext& factory_context) {
  return [&factory_context](Http::FilterChainFactoryCallbacks& callbacks) {
    callbacks.addStreamFilter(std::make_shared<ConnectWebFilter>(factory_context.connectContext()));
  };
}

/**
 * Static registration for the Connect-Web filter. @see RegisterFactory.
 */
REGISTER_FACTORY(ConnectWebFilterConfig,
                 Server::Configuration::NamedHttpFilterConfigFactory){"envoy.connect_web"};

} // namespace ConnectWeb
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
