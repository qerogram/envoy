load(
    "//bazel:envoy_build_system.bzl",
    "envoy_cc_test",
    "envoy_package",
)

licenses(["notice"])  # Apache 2

envoy_package()

envoy_cc_test(
    name = "factory_test",
    srcs = ["factory_test.cc"],
    data = [
        "//test/extensions/dynamic_modules/test_data/c:http_filter_per_route_config_new_fail",
        "//test/extensions/dynamic_modules/test_data/c:no_http_config_destroy",
        "//test/extensions/dynamic_modules/test_data/c:no_http_config_new",
        "//test/extensions/dynamic_modules/test_data/c:no_http_filter_destroy",
        "//test/extensions/dynamic_modules/test_data/c:no_http_filter_new",
        "//test/extensions/dynamic_modules/test_data/c:no_http_filter_per_route_config_destroy",
        "//test/extensions/dynamic_modules/test_data/c:no_http_filter_per_route_config_new",
        "//test/extensions/dynamic_modules/test_data/c:no_http_filter_request_body",
        "//test/extensions/dynamic_modules/test_data/c:no_http_filter_request_headers",
        "//test/extensions/dynamic_modules/test_data/c:no_http_filter_request_trailers",
        "//test/extensions/dynamic_modules/test_data/c:no_http_filter_response_body",
        "//test/extensions/dynamic_modules/test_data/c:no_http_filter_response_headers",
        "//test/extensions/dynamic_modules/test_data/c:no_http_filter_response_trailers",
        "//test/extensions/dynamic_modules/test_data/c:no_http_filter_stream_complete",
        "//test/extensions/dynamic_modules/test_data/c:no_op",
    ],
    # factory_context_mocks needs this.
    rbe_pool = "6gig",
    deps = [
        "//source/extensions/filters/http/dynamic_modules:factory_lib",
        "//test/extensions/dynamic_modules:util",
        "//test/mocks/server:factory_context_mocks",
    ],
)

envoy_cc_test(
    name = "filter_test",
    srcs = ["filter_test.cc"],
    data = [
        "//test/extensions/dynamic_modules/test_data/c:no_op",
        "//test/extensions/dynamic_modules/test_data/rust:http",
        "//test/extensions/dynamic_modules/test_data/rust:http_integration_test",
        "//test/extensions/dynamic_modules/test_data/rust:no_op",
    ],
    # http_mocks needs this.
    rbe_pool = "6gig",
    deps = [
        "//source/common/router:string_accessor_lib",
        "//source/extensions/filters/http/dynamic_modules:abi_impl",
        "//source/extensions/filters/http/dynamic_modules:filter_lib",
        "//test/extensions/dynamic_modules:util",
        "//test/mocks/http:http_mocks",
        "//test/mocks/server:server_factory_context_mocks",
        "//test/mocks/upstream:cluster_manager_mocks",
        "//test/mocks/upstream:thread_local_cluster_mocks",
    ],
)

envoy_cc_test(
    name = "abi_impl_test",
    srcs = ["abi_impl_test.cc"],
    # http_mocks needs this.
    rbe_pool = "6gig",
    deps = [
        "//source/extensions/filters/http/dynamic_modules:abi_impl",
        "//test/mocks/http:http_mocks",
        "//test/mocks/network:network_mocks",
        "//test/mocks/ssl:ssl_mocks",
    ],
)

envoy_cc_test(
    name = "integration_test",
    srcs = ["integration_test.cc"],
    data = [
        "//test/extensions/dynamic_modules/test_data/rust:http_integration_test",
    ],
    rbe_pool = "6gig",
    deps = [
        "//source/extensions/filters/http/dynamic_modules:factory_registration",
        "//test/integration:http_integration_lib",
        "@envoy_api//envoy/extensions/filters/http/dynamic_modules/v3:pkg_cc_proto",
    ],
)
