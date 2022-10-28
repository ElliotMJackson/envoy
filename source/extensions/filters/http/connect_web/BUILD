load(
    "//bazel:envoy_build_system.bzl",
    "envoy_cc_extension",
    "envoy_cc_library",
    "envoy_extension_package",
)

licenses(["notice"])  # Apache 2

# L7 HTTP filter that implements the connect-web protocol (https://github.com/connect/connect/blob/master/doc/PROTOCOL-WEB.md)
# Public docs: https://envoyproxy.io/docs/envoy/latest/configuration/http/http_filters/connect_web_filter

envoy_extension_package()

envoy_cc_library(
    name = "connect_web_filter_lib",
    srcs = ["connect_web_filter.cc"],
    hdrs = ["connect_web_filter.h"],
    deps = [
        "//envoy/http:filter_interface",
        "//envoy/upstream:cluster_manager_interface",
        "//source/common/common:base64_lib",
        "//source/common/common:utility_lib",
        "//source/common/connect:codec_lib",
        "//source/common/connect:context_lib",
        "//source/common/http:headers_lib",
        "//source/common/http:utility_lib",
    ],
)

envoy_cc_extension(
    name = "config",
    srcs = ["config.cc"],
    hdrs = ["config.h"],
    deps = [
        "//envoy/registry",
        "//envoy/server:filter_config_interface",
        "//source/extensions/filters/http/common:factory_base_lib",
        "//source/extensions/filters/http/connect_web:connect_web_filter_lib",
        "@envoy_api//envoy/extensions/filters/http/connect_web/v3:pkg_cc_proto",
    ],
)
