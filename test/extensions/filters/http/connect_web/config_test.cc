#include "source/extensions/filters/http/connect_web/config.h"

#include "test/mocks/server/factory_context.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

using testing::_;

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace ConnectWeb {
namespace {

TEST(ConnectWebFilterConfigTest, ConnectWebFilter) {
  NiceMock<Server::Configuration::MockFactoryContext> context;
  ConnectWebFilterConfig factory;
  envoy::extensions::filters::http::connect_web::v3::ConnectWeb config;
  Http::FilterFactoryCb cb = factory.createFilterFactoryFromProto(config, "stats", context);
  Http::MockFilterChainFactoryCallbacks filter_callback;
  EXPECT_CALL(filter_callback, addStreamFilter(_));
  cb(filter_callback);
}

} // namespace
} // namespace ConnectWeb
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
