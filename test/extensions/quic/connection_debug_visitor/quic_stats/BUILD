load(
    "//bazel:envoy_build_system.bzl",
    "envoy_package",
    "envoy_select_enable_http3",
)
load(
    "//test/extensions:extensions_build_system.bzl",
    "envoy_extension_cc_test",
)

licenses(["notice"])  # Apache 2

envoy_package()

envoy_extension_cc_test(
    name = "quic_stats_test",
    srcs = envoy_select_enable_http3(["quic_stats_test.cc"]),
    extension_names = ["envoy.quic.connection_debug_visitor.quic_stats"],
    deps = envoy_select_enable_http3([
        "//source/extensions/quic/connection_debug_visitor/quic_stats:quic_stats_lib",
        "//test/mocks/event:event_mocks",
    ]),
)

envoy_extension_cc_test(
    name = "integration_test",
    size = "large",
    srcs = envoy_select_enable_http3(["integration_test.cc"]),
    extension_names = ["envoy.quic.connection_debug_visitor.quic_stats"],
    rbe_pool = "2core",
    deps = envoy_select_enable_http3([
        "//source/extensions/quic/connection_debug_visitor/quic_stats:config",
        "//test/integration:http_integration_lib",
        "@envoy_api//envoy/extensions/quic/connection_debug_visitor/quic_stats/v3:pkg_cc_proto",
        "@envoy_api//envoy/extensions/transport_sockets/quic/v3:pkg_cc_proto",
    ]),
)
