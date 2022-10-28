load(
    "//bazel:envoy_build_system.bzl",
    "envoy_package",
)
load(
    "//test/extensions:extensions_build_system.bzl",
    "envoy_extension_cc_test",
)

licenses(["notice"])  # Apache 2

envoy_package()

envoy_extension_cc_test(
    name = "connect_web_filter_test",
    srcs = ["connect_web_filter_test.cc"],
    extension_names = ["envoy.filters.http.connect_web"],
    deps = [
        "//source/extensions/filters/http/connect_web:connect_web_filter_lib",
        "//test/mocks/http:http_mocks",
        "//test/test_common:global_lib",
        "//test/test_common:utility_lib",
    ],
)

envoy_extension_cc_test(
    name = "config_test",
    srcs = ["config_test.cc"],
    extension_names = ["envoy.filters.http.connect_web"],
    deps = [
        "//source/extensions/filters/http/connect_web:config",
        "//test/mocks/server:factory_context_mocks",
    ],
)

envoy_extension_cc_test(
    name = "connect_web_integration_test",
    srcs = ["connect_web_filter_integration_test.cc"],
    extension_names = ["envoy.filters.http.connect_web"],
    deps = [
        "//source/common/buffer:buffer_lib",
        "//source/common/http:header_map_lib",
        "//source/extensions/filters/http/connect_web:config",
        "//source/extensions/filters/http/connect_web:connect_web_filter_lib",
        "//test/integration:http_integration_lib",
        "//test/test_common:utility_lib",
    ],
)
