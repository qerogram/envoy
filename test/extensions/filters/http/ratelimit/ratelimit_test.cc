#include <memory>
#include <string>
#include <vector>

#include "envoy/extensions/filters/http/ratelimit/v3/rate_limit.pb.h"
#include "envoy/stream_info/stream_info.h"

#include "source/common/buffer/buffer_impl.h"
#include "source/common/common/empty_string.h"
#include "source/common/http/context_impl.h"
#include "source/common/http/headers.h"
#include "source/common/stream_info/uint32_accessor_impl.h"
#include "source/extensions/filters/http/ratelimit/ratelimit.h"

#include "test/extensions/filters/common/ratelimit/mocks.h"
#include "test/extensions/filters/common/ratelimit/utils.h"
#include "test/mocks/http/mocks.h"
#include "test/mocks/local_info/mocks.h"
#include "test/mocks/ratelimit/mocks.h"
#include "test/mocks/runtime/mocks.h"
#include "test/mocks/server/server_factory_context.h"
#include "test/mocks/tracing/mocks.h"
#include "test/test_common/printers.h"
#include "test/test_common/utility.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

using testing::_;
using testing::InSequence;
using testing::Invoke;
using testing::NiceMock;
using testing::Not;
using testing::Return;
using testing::SetArgReferee;
using testing::WithArgs;

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace RateLimitFilter {
namespace {

class HttpRateLimitFilterTest : public testing::Test {
public:
  HttpRateLimitFilterTest() {
    ON_CALL(factory_context_.runtime_loader_.snapshot_,
            featureEnabled("ratelimit.http_filter_enabled", 100))
        .WillByDefault(Return(true));
    ON_CALL(factory_context_.runtime_loader_.snapshot_,
            featureEnabled("ratelimit.http_filter_enforcing", 100))
        .WillByDefault(Return(true));
    ON_CALL(factory_context_.runtime_loader_.snapshot_,
            featureEnabled("ratelimit.test_key.http_filter_enabled", 100))
        .WillByDefault(Return(true));
  }

  void setUpTest(const std::string& yaml, const std::string& route_config_yaml = "") {
    envoy::extensions::filters::http::ratelimit::v3::RateLimit proto_config{};
    TestUtility::loadFromYaml(yaml, proto_config);

    auto status = absl::OkStatus();
    config_ = std::make_shared<FilterConfig>(
        proto_config, factory_context_.local_info_, *factory_context_.store_.rootScope(),
        factory_context_.runtime_loader_, factory_context_.http_context_, status);
    EXPECT_TRUE(status.ok());

    client_ = new Filters::Common::RateLimit::MockClient();
    filter_ = std::make_unique<Filter>(config_, Filters::Common::RateLimit::ClientPtr{client_});
    filter_->setDecoderFilterCallbacks(filter_callbacks_);
    filter_callbacks_.route_->route_entry_.rate_limit_policy_.rate_limit_policy_entry_.clear();
    filter_callbacks_.route_->route_entry_.rate_limit_policy_.rate_limit_policy_entry_.emplace_back(
        route_rate_limit_);
    filter_callbacks_.route_->virtual_host_->rate_limit_policy_.rate_limit_policy_entry_.clear();
    filter_callbacks_.route_->virtual_host_->rate_limit_policy_.rate_limit_policy_entry_
        .emplace_back(vh_rate_limit_);

    if (!route_config_yaml.empty()) {
      envoy::extensions::filters::http::ratelimit::v3::RateLimitPerRoute settings;
      TestUtility::loadFromYaml(route_config_yaml, settings);
      absl::Status creation_status = absl::OkStatus();
      route_config_ =
          std::make_shared<FilterConfigPerRoute>(factory_context_, settings, creation_status);
      EXPECT_TRUE(creation_status.ok());

      EXPECT_CALL(filter_callbacks_, mostSpecificPerFilterConfig())
          .WillOnce(Return(route_config_.get()));
    }
  }

  const std::string fail_close_config_ = R"EOF(
  domain: foo
  failure_mode_deny: true
  )EOF";

  const std::string fail_close_with_custom_status_code_config_ = R"EOF(
  domain: foo
  failure_mode_deny: true
  status_on_error:
    code: 503
  )EOF";

  const std::string enable_x_ratelimit_headers_config_ = R"EOF(
  domain: foo
  enable_x_ratelimit_headers: DRAFT_VERSION_03
  )EOF";

  const std::string disable_x_envoy_ratelimited_header_config_ = R"EOF(
  domain: foo
  disable_x_envoy_ratelimited_header: true
  )EOF";

  const std::string filter_config_ = R"EOF(
  domain: foo
  )EOF";

  const std::string rate_limited_status_config_ = R"EOF(
  domain: foo
  rate_limited_status:
    code: 503
  )EOF";

  const std::string invalid_rate_limited_status_config_ = R"EOF(
  domain: foo
  rate_limited_status:
    code: 200
  )EOF";

  const std::string stat_prefix_config_ = R"EOF(
  domain: foo
  stat_prefix: with_stat_prefix
  )EOF";

  const std::string filter_config_with_filter_enabled_ = R"EOF(
  domain: foo
  filter_enabled:
    runtime_key: test_enabled
    default_value:
      numerator: 30
      denominator: HUNDRED
  )EOF";

  const std::string filter_config_with_filter_enforced_ = R"EOF(
    domain: foo
    filter_enforced:
      runtime_key: test_enforced
      default_value:
        numerator: 50
        denominator: HUNDRED
    )EOF";

  const std::string failure_mode_runtime_zero_percent_config_ = R"EOF(
  domain: foo
  rate_limit_service:
    grpc_service:
      envoy_grpc:
        cluster_name: ratelimit
      timeout: 0.25s
  failure_mode_deny_percent:
    runtime_key: test.ratelimit.failure_mode_deny_percent
    default_value:
      numerator: 0
      denominator: HUNDRED
  )EOF";

  const std::string failure_mode_runtime_hundred_percent_config_ = R"EOF(
  domain: foo
  rate_limit_service:
    grpc_service:
      envoy_grpc:
        cluster_name: ratelimit
      timeout: 0.25s
  failure_mode_deny_percent:
    runtime_key: test.ratelimit.failure_mode_deny_percent
    default_value:
      numerator: 100
      denominator: HUNDRED
  )EOF";

  Filters::Common::RateLimit::MockClient* client_;
  NiceMock<Http::MockStreamDecoderFilterCallbacks> filter_callbacks_;
  Stats::StatNamePool pool_{filter_callbacks_.clusterInfo()->statsScope().symbolTable()};
  Stats::StatName ratelimit_ok_{pool_.add("ratelimit.ok")};
  Stats::StatName ratelimit_error_{pool_.add("ratelimit.error")};
  Stats::StatName ratelimit_failure_mode_allowed_{pool_.add("ratelimit.failure_mode_allowed")};
  Stats::StatName ratelimit_over_limit_{pool_.add("ratelimit.over_limit")};
  Stats::StatName upstream_rq_4xx_{pool_.add("upstream_rq_4xx")};
  Stats::StatName upstream_rq_429_{pool_.add("upstream_rq_429")};
  Stats::StatName upstream_rq_5xx_{pool_.add("upstream_rq_5xx")};
  Stats::StatName upstream_rq_503_{pool_.add("upstream_rq_503")};
  Filters::Common::RateLimit::RequestCallbacks* request_callbacks_{};
  Http::TestRequestHeaderMapImpl request_headers_;
  Http::TestRequestTrailerMapImpl request_trailers_;
  Http::TestResponseHeaderMapImpl response_headers_;
  Http::TestResponseTrailerMapImpl response_trailers_;
  Buffer::OwnedImpl data_;
  Buffer::OwnedImpl response_data_;
  FilterConfigSharedPtr config_;
  FilterConfigPerRouteSharedPtr route_config_;
  std::unique_ptr<Filter> filter_;
  NiceMock<Router::MockRateLimitPolicyEntry> route_rate_limit_;
  NiceMock<Router::MockRateLimitPolicyEntry> vh_rate_limit_;
  std::vector<RateLimit::Descriptor> descriptor_{{{{"descriptor_key", "descriptor_value"}}}};
  std::vector<RateLimit::Descriptor> descriptor_two_{{{{"key", "value"}}}};
  NiceMock<Server::Configuration::MockServerFactoryContext> factory_context_;
};

TEST_F(HttpRateLimitFilterTest, NoRoute) {
  setUpTest(filter_config_);

  EXPECT_CALL(*filter_callbacks_.route_, routeEntry()).WillOnce(Return(nullptr));

  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers_, false));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->decodeData(data_, false));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->decodeTrailers(request_trailers_));
  EXPECT_EQ(Http::Filter1xxHeadersStatus::Continue, filter_->encode1xxHeaders(response_headers_));
  Http::MetadataMap metadata_map{{"metadata", "metadata"}};
  EXPECT_EQ(Http::FilterMetadataStatus::Continue, filter_->encodeMetadata(metadata_map));
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers_, false));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(response_data_, false));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->encodeTrailers(response_trailers_));
}

TEST_F(HttpRateLimitFilterTest, NoCluster) {
  setUpTest(filter_config_);

  ON_CALL(filter_callbacks_, clusterInfo()).WillByDefault(Return(nullptr));

  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers_, false));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->decodeData(data_, false));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->decodeTrailers(request_trailers_));
  EXPECT_EQ(Http::Filter1xxHeadersStatus::Continue, filter_->encode1xxHeaders(response_headers_));
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers_, false));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(response_data_, false));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->encodeTrailers(response_trailers_));
}

TEST_F(HttpRateLimitFilterTest, NoApplicableRateLimit) {
  setUpTest(filter_config_);

  filter_callbacks_.route_->route_entry_.rate_limit_policy_.rate_limit_policy_entry_.clear();
  EXPECT_CALL(*client_, limit(_, _, _, _, _, 0)).Times(0);
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers_, false));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->decodeData(data_, false));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->decodeTrailers(request_trailers_));
  EXPECT_EQ(Http::Filter1xxHeadersStatus::Continue, filter_->encode1xxHeaders(response_headers_));
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers_, false));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(response_data_, false));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->encodeTrailers(response_trailers_));
}

TEST_F(HttpRateLimitFilterTest, NoDescriptor) {
  setUpTest(filter_config_);

  EXPECT_CALL(route_rate_limit_, populateDescriptors(_, _, _, _));
  EXPECT_CALL(vh_rate_limit_, populateDescriptors(_, _, _, _));
  EXPECT_CALL(*client_, limit(_, _, _, _, _, 0)).Times(0);
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers_, false));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->decodeData(data_, false));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->decodeTrailers(request_trailers_));
  EXPECT_EQ(Http::Filter1xxHeadersStatus::Continue, filter_->encode1xxHeaders(response_headers_));
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers_, false));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(response_data_, false));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->encodeTrailers(response_trailers_));
}

TEST_F(HttpRateLimitFilterTest, RuntimeDisabled) {
  setUpTest(filter_config_);

  EXPECT_CALL(factory_context_.runtime_loader_.snapshot_,
              featureEnabled("ratelimit.http_filter_enabled", 100))
      .WillOnce(Return(false));
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers_, false));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->decodeData(data_, false));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->decodeTrailers(request_trailers_));
  EXPECT_EQ(Http::Filter1xxHeadersStatus::Continue, filter_->encode1xxHeaders(response_headers_));
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers_, false));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(response_data_, false));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->encodeTrailers(response_trailers_));
}

TEST_F(HttpRateLimitFilterTest, RuntimeDisabledFromFilterConfig) {
  setUpTest(filter_config_with_filter_enabled_);

  EXPECT_CALL(
      factory_context_.runtime_loader_.snapshot_,
      featureEnabled(absl::string_view("test_enabled"),
                     testing::Matcher<const envoy::type::v3::FractionalPercent&>(Percent(30))))
      .WillOnce(testing::Return(false));

  // Explicit configuration in the filter config should override the default runtime key.
  EXPECT_CALL(factory_context_.runtime_loader_.snapshot_,
              featureEnabled("ratelimit.http_filter_enabled", 100))
      .Times(0);

  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers_, false));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->decodeData(data_, false));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->decodeTrailers(request_trailers_));
  EXPECT_EQ(Http::Filter1xxHeadersStatus::Continue, filter_->encode1xxHeaders(response_headers_));
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers_, false));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(response_data_, false));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->encodeTrailers(response_trailers_));
}

TEST_F(HttpRateLimitFilterTest, OkResponse) {
  setUpTest(filter_config_);
  InSequence s;

  EXPECT_CALL(filter_callbacks_.route_->route_entry_.rate_limit_policy_, getApplicableRateLimit(0));

  EXPECT_CALL(route_rate_limit_, populateDescriptors(_, _, _, _))
      .WillOnce(SetArgReferee<0>(descriptor_));

  EXPECT_CALL(filter_callbacks_.route_->virtual_host_->rate_limit_policy_,
              getApplicableRateLimit(0));

  EXPECT_CALL(*client_, limit(_, "foo",
                              testing::ContainerEq(std::vector<RateLimit::Descriptor>{
                                  {{{"descriptor_key", "descriptor_value"}}}}),
                              _, _, 0))
      .WillOnce(
          WithArgs<0>(Invoke([&](Filters::Common::RateLimit::RequestCallbacks& callbacks) -> void {
            request_callbacks_ = &callbacks;
          })));

  request_headers_.addCopy(Http::Headers::get().RequestId, "requestid");
  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(request_headers_, false));
  Http::MetadataMap metadata_map{{"metadata", "metadata"}};
  EXPECT_EQ(Http::FilterMetadataStatus::Continue, filter_->decodeMetadata(metadata_map));
  EXPECT_EQ(Http::FilterDataStatus::StopIterationAndWatermark, filter_->decodeData(data_, false));
  EXPECT_EQ(Http::FilterTrailersStatus::StopIteration, filter_->decodeTrailers(request_trailers_));
  EXPECT_EQ(Http::Filter1xxHeadersStatus::Continue, filter_->encode1xxHeaders(response_headers_));
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers_, false));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(response_data_, false));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->encodeTrailers(response_trailers_));

  EXPECT_CALL(filter_callbacks_, continueDecoding());
  EXPECT_CALL(filter_callbacks_.stream_info_,
              setResponseFlag(StreamInfo::CoreResponseFlag::RateLimited))
      .Times(0);
  request_callbacks_->complete(Filters::Common::RateLimit::LimitStatus::OK, nullptr, nullptr,
                               nullptr, "", nullptr);

  EXPECT_EQ(
      1U, filter_callbacks_.clusterInfo()->statsScope().counterFromStatName(ratelimit_ok_).value());
}

TEST_F(HttpRateLimitFilterTest, OkResponseWithAdditionalHitsAddend) {
  setUpTest(filter_config_);
  InSequence s;

  filter_callbacks_.stream_info_.filter_state_->setData(
      "envoy.ratelimit.hits_addend", std::make_unique<StreamInfo::UInt32AccessorImpl>(5),
      StreamInfo::FilterState::StateType::Mutable);
  EXPECT_CALL(filter_callbacks_.route_->route_entry_.rate_limit_policy_, getApplicableRateLimit(0));

  EXPECT_CALL(route_rate_limit_, populateDescriptors(_, _, _, _))
      .WillOnce(SetArgReferee<0>(descriptor_));

  EXPECT_CALL(vh_rate_limit_, applyOnStreamDone()).WillRepeatedly(Return(true));
  EXPECT_CALL(*client_, limit(_, "foo",
                              testing::ContainerEq(std::vector<RateLimit::Descriptor>{
                                  {{{"descriptor_key", "descriptor_value"}}}}),
                              _, _, 5))
      .WillOnce(
          WithArgs<0>(Invoke([&](Filters::Common::RateLimit::RequestCallbacks& callbacks) -> void {
            request_callbacks_ = &callbacks;
          })));

  request_headers_.addCopy(Http::Headers::get().RequestId, "requestid");
  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(request_headers_, false));
  Http::MetadataMap metadata_map{{"metadata", "metadata"}};
  EXPECT_EQ(Http::FilterMetadataStatus::Continue, filter_->decodeMetadata(metadata_map));
  EXPECT_EQ(Http::FilterDataStatus::StopIterationAndWatermark, filter_->decodeData(data_, false));
  EXPECT_EQ(Http::FilterTrailersStatus::StopIteration, filter_->decodeTrailers(request_trailers_));
  EXPECT_EQ(Http::Filter1xxHeadersStatus::Continue, filter_->encode1xxHeaders(response_headers_));
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers_, false));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(response_data_, false));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->encodeTrailers(response_trailers_));

  EXPECT_CALL(filter_callbacks_, continueDecoding());
  EXPECT_CALL(filter_callbacks_.stream_info_,
              setResponseFlag(StreamInfo::CoreResponseFlag::RateLimited))
      .Times(0);
  request_callbacks_->complete(Filters::Common::RateLimit::LimitStatus::OK, nullptr, nullptr,
                               nullptr, "", nullptr);

  EXPECT_EQ(
      1U, filter_callbacks_.clusterInfo()->statsScope().counterFromStatName(ratelimit_ok_).value());

  // Test the behavior for the apply_on_stream_done flag.
  testing::Mock::VerifyAndClearExpectations(client_);
  testing::Mock::VerifyAndClearExpectations(&filter_callbacks_);
  testing::Mock::VerifyAndClearExpectations(
      &filter_callbacks_.route_->route_entry_.rate_limit_policy_);
  testing::Mock::VerifyAndClearExpectations(&route_rate_limit_);
  testing::Mock::VerifyAndClearExpectations(&vh_rate_limit_);
  filter_callbacks_.stream_info_.filter_state_->setData(
      // Ensures that addend can be set differently than the request path.
      "envoy.ratelimit.hits_addend", std::make_unique<StreamInfo::UInt32AccessorImpl>(100),
      StreamInfo::FilterState::StateType::Mutable);
  EXPECT_CALL(filter_callbacks_.route_->route_entry_.rate_limit_policy_, getApplicableRateLimit(0));
  EXPECT_CALL(vh_rate_limit_, applyOnStreamDone()).WillRepeatedly(Return(true));
  EXPECT_CALL(vh_rate_limit_, populateDescriptors(_, _, _, _))
      .WillOnce(SetArgReferee<0>(descriptor_two_));
  EXPECT_CALL(*client_, limit(_, "foo", testing::ContainerEq(descriptor_two_), _, _, 100))
      .WillOnce(
          WithArgs<0>(Invoke([&](Filters::Common::RateLimit::RequestCallbacks& callbacks) -> void {
            request_callbacks_ = &callbacks;
          })));
  filter_->onDestroy();
  request_callbacks_->complete(Filters::Common::RateLimit::LimitStatus::OK, nullptr, nullptr,
                               nullptr, "", nullptr);
}

TEST_F(HttpRateLimitFilterTest, OkResponseWithHeaders) {
  setUpTest(filter_config_);
  InSequence s;

  EXPECT_CALL(filter_callbacks_.route_->route_entry_.rate_limit_policy_, getApplicableRateLimit(0));

  EXPECT_CALL(route_rate_limit_, populateDescriptors(_, _, _, _))
      .WillOnce(SetArgReferee<0>(descriptor_));

  EXPECT_CALL(filter_callbacks_.route_->virtual_host_->rate_limit_policy_,
              getApplicableRateLimit(0));

  EXPECT_CALL(*client_, limit(_, "foo",
                              testing::ContainerEq(std::vector<RateLimit::Descriptor>{
                                  {{{"descriptor_key", "descriptor_value"}}}}),
                              _, _, 0))
      .WillOnce(
          WithArgs<0>(Invoke([&](Filters::Common::RateLimit::RequestCallbacks& callbacks) -> void {
            request_callbacks_ = &callbacks;
          })));

  request_headers_.addCopy(Http::Headers::get().RequestId, "requestid");
  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(request_headers_, false));
  EXPECT_EQ(Http::FilterDataStatus::StopIterationAndWatermark, filter_->decodeData(data_, false));
  EXPECT_EQ(Http::FilterTrailersStatus::StopIteration, filter_->decodeTrailers(request_trailers_));
  EXPECT_EQ(Http::Filter1xxHeadersStatus::Continue, filter_->encode1xxHeaders(response_headers_));
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers_, false));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(response_data_, false));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->encodeTrailers(response_trailers_));

  EXPECT_CALL(filter_callbacks_, continueDecoding());
  EXPECT_CALL(filter_callbacks_.stream_info_,
              setResponseFlag(StreamInfo::CoreResponseFlag::RateLimited))
      .Times(0);

  Http::HeaderMapPtr request_headers_to_add{
      new Http::TestRequestHeaderMapImpl{{"x-rls-rate-limited", "true"}}};
  Http::HeaderMapPtr rl_headers{new Http::TestResponseHeaderMapImpl{
      {"x-ratelimit-limit", "1000"}, {"x-ratelimit-remaining", "500"}}};

  request_callbacks_->complete(
      Filters::Common::RateLimit::LimitStatus::OK, nullptr,
      Http::ResponseHeaderMapPtr{new Http::TestResponseHeaderMapImpl(*rl_headers)},
      Http::RequestHeaderMapPtr{new Http::TestRequestHeaderMapImpl(*request_headers_to_add)}, "",
      nullptr);
  Http::TestResponseHeaderMapImpl expected_headers(*rl_headers);
  Http::TestResponseHeaderMapImpl response_headers;
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers, false));
  EXPECT_EQ(true, (expected_headers == response_headers));

  EXPECT_THAT(*request_headers_to_add, IsSubsetOfHeaders(request_headers_));
  EXPECT_EQ(
      1U, filter_callbacks_.clusterInfo()->statsScope().counterFromStatName(ratelimit_ok_).value());
}

TEST_F(HttpRateLimitFilterTest, OkResponseWithFilterHeaders) {
  setUpTest(enable_x_ratelimit_headers_config_);
  InSequence s;

  EXPECT_CALL(filter_callbacks_.route_->route_entry_.rate_limit_policy_, getApplicableRateLimit(0));

  EXPECT_CALL(route_rate_limit_, populateDescriptors(_, _, _, _))
      .WillOnce(SetArgReferee<0>(descriptor_));

  EXPECT_CALL(filter_callbacks_.route_->virtual_host_->rate_limit_policy_,
              getApplicableRateLimit(0));

  EXPECT_CALL(*client_, limit(_, "foo",
                              testing::ContainerEq(std::vector<RateLimit::Descriptor>{
                                  {{{"descriptor_key", "descriptor_value"}}}}),
                              _, _, 0))
      .WillOnce(
          WithArgs<0>(Invoke([&](Filters::Common::RateLimit::RequestCallbacks& callbacks) -> void {
            request_callbacks_ = &callbacks;
          })));

  request_headers_.addCopy(Http::Headers::get().RequestId, "requestid");
  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(request_headers_, false));
  EXPECT_EQ(Http::FilterDataStatus::StopIterationAndWatermark, filter_->decodeData(data_, false));
  EXPECT_EQ(Http::FilterTrailersStatus::StopIteration, filter_->decodeTrailers(request_trailers_));
  EXPECT_EQ(Http::Filter1xxHeadersStatus::Continue, filter_->encode1xxHeaders(response_headers_));
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers_, false));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(response_data_, false));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->encodeTrailers(response_trailers_));

  EXPECT_CALL(filter_callbacks_, continueDecoding());
  EXPECT_CALL(filter_callbacks_.stream_info_,
              setResponseFlag(StreamInfo::CoreResponseFlag::RateLimited))
      .Times(0);

  auto descriptor_statuses = {
      Envoy::RateLimit::buildDescriptorStatus(
          1, envoy::service::ratelimit::v3::RateLimitResponse::RateLimit::MINUTE, "first", 2, 3),
      Envoy::RateLimit::buildDescriptorStatus(
          4, envoy::service::ratelimit::v3::RateLimitResponse::RateLimit::HOUR, "second", 5, 6)};
  auto descriptor_statuses_ptr =
      std::make_unique<Filters::Common::RateLimit::DescriptorStatusList>(descriptor_statuses);
  request_callbacks_->complete(Filters::Common::RateLimit::LimitStatus::OK,
                               std::move(descriptor_statuses_ptr), nullptr, nullptr, "", nullptr);

  Http::TestResponseHeaderMapImpl expected_headers{
      {"x-ratelimit-limit", "1, 1;w=60;name=\"first\", 4;w=3600;name=\"second\""},
      {"x-ratelimit-remaining", "2"},
      {"x-ratelimit-reset", "3"}};
  Http::TestResponseHeaderMapImpl response_headers;
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers, false));
  EXPECT_THAT(response_headers, HeaderMapEqualRef(&expected_headers));
  EXPECT_EQ(
      1U, filter_callbacks_.clusterInfo()->statsScope().counterFromStatName(ratelimit_ok_).value());
}

TEST_F(HttpRateLimitFilterTest, ImmediateOkResponse) {
  setUpTest(filter_config_);
  InSequence s;

  EXPECT_CALL(vh_rate_limit_, populateDescriptors(_, _, _, _))
      .WillOnce(SetArgReferee<0>(descriptor_));

  EXPECT_CALL(*client_, limit(_, "foo",
                              testing::ContainerEq(std::vector<RateLimit::Descriptor>{
                                  {{{"descriptor_key", "descriptor_value"}}}}),
                              _, _, 0))
      .WillOnce(
          WithArgs<0>(Invoke([&](Filters::Common::RateLimit::RequestCallbacks& callbacks) -> void {
            callbacks.complete(Filters::Common::RateLimit::LimitStatus::OK, nullptr, nullptr,
                               nullptr, "", nullptr);
          })));

  EXPECT_CALL(filter_callbacks_, continueDecoding()).Times(0);
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers_, false));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->decodeData(data_, false));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->decodeTrailers(request_trailers_));
  EXPECT_EQ(Http::Filter1xxHeadersStatus::Continue, filter_->encode1xxHeaders(response_headers_));
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers_, false));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(response_data_, false));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->encodeTrailers(response_trailers_));

  EXPECT_EQ(
      1U, filter_callbacks_.clusterInfo()->statsScope().counterFromStatName(ratelimit_ok_).value());
}

TEST_F(HttpRateLimitFilterTest, ImmediateErrorResponse) {
  setUpTest(filter_config_);
  InSequence s;

  EXPECT_CALL(vh_rate_limit_, populateDescriptors(_, _, _, _))
      .WillOnce(SetArgReferee<0>(descriptor_));

  EXPECT_CALL(*client_, limit(_, "foo",
                              testing::ContainerEq(std::vector<RateLimit::Descriptor>{
                                  {{{"descriptor_key", "descriptor_value"}}}}),
                              _, _, 0))
      .WillOnce(
          WithArgs<0>(Invoke([&](Filters::Common::RateLimit::RequestCallbacks& callbacks) -> void {
            callbacks.complete(Filters::Common::RateLimit::LimitStatus::Error, nullptr, nullptr,
                               nullptr, "", nullptr);
          })));

  EXPECT_CALL(filter_callbacks_, continueDecoding()).Times(0);
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers_, false));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->decodeData(data_, false));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->decodeTrailers(request_trailers_));
  EXPECT_EQ(Http::Filter1xxHeadersStatus::Continue, filter_->encode1xxHeaders(response_headers_));
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers_, false));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(response_data_, false));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->encodeTrailers(response_trailers_));

  EXPECT_EQ(
      1U,
      filter_callbacks_.clusterInfo()->statsScope().counterFromStatName(ratelimit_error_).value());
  EXPECT_EQ(1U, filter_callbacks_.clusterInfo()
                    ->statsScope()
                    .counterFromStatName(ratelimit_failure_mode_allowed_)
                    .value());
}

TEST_F(HttpRateLimitFilterTest, ErrorResponse) {
  setUpTest(filter_config_);
  InSequence s;

  EXPECT_CALL(route_rate_limit_, populateDescriptors(_, _, _, _))
      .WillOnce(SetArgReferee<0>(descriptor_));
  EXPECT_CALL(*client_, limit(_, _, _, _, _, 0))
      .WillOnce(
          WithArgs<0>(Invoke([&](Filters::Common::RateLimit::RequestCallbacks& callbacks) -> void {
            request_callbacks_ = &callbacks;
          })));

  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(request_headers_, false));

  EXPECT_CALL(filter_callbacks_, continueDecoding());
  request_callbacks_->complete(Filters::Common::RateLimit::LimitStatus::Error, nullptr, nullptr,
                               nullptr, "", nullptr);

  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->decodeData(data_, false));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->decodeTrailers(request_trailers_));
  EXPECT_CALL(filter_callbacks_.stream_info_,
              setResponseFlag(StreamInfo::CoreResponseFlag::RateLimited))
      .Times(0);

  EXPECT_EQ(
      1U,
      filter_callbacks_.clusterInfo()->statsScope().counterFromStatName(ratelimit_error_).value());
  EXPECT_EQ(1U, filter_callbacks_.clusterInfo()
                    ->statsScope()
                    .counterFromStatName(ratelimit_failure_mode_allowed_)
                    .value());
}

TEST_F(HttpRateLimitFilterTest, ErrorResponseWithFailureModeAllowOff) {
  setUpTest(fail_close_config_);
  InSequence s;

  EXPECT_CALL(route_rate_limit_, populateDescriptors(_, _, _, _))
      .WillOnce(SetArgReferee<0>(descriptor_));
  EXPECT_CALL(*client_, limit(_, _, _, _, _, 0))
      .WillOnce(
          WithArgs<0>(Invoke([&](Filters::Common::RateLimit::RequestCallbacks& callbacks) -> void {
            request_callbacks_ = &callbacks;
          })));

  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(request_headers_, false));

  EXPECT_CALL(filter_callbacks_.stream_info_,
              setResponseFlag(StreamInfo::CoreResponseFlag::RateLimitServiceError));

  EXPECT_CALL(filter_callbacks_, encodeHeaders_(_, true))
      .WillOnce(Invoke([&](const Http::ResponseHeaderMap& headers, bool) -> void {
        EXPECT_EQ(headers.getStatusValue(),
                  std::to_string(enumToInt(Http::Code::InternalServerError)));
      }));

  request_callbacks_->complete(Filters::Common::RateLimit::LimitStatus::Error, nullptr, nullptr,
                               nullptr, "", nullptr);

  EXPECT_EQ(
      1U,
      filter_callbacks_.clusterInfo()->statsScope().counterFromStatName(ratelimit_error_).value());
  EXPECT_EQ(0U, filter_callbacks_.clusterInfo()
                    ->statsScope()
                    .counterFromStatName(ratelimit_failure_mode_allowed_)
                    .value());
  EXPECT_EQ("rate_limiter_error", filter_callbacks_.details());
}

TEST_F(HttpRateLimitFilterTest, ErrorResponseWithFailureModeAllowOffAndCustomStatusOn) {
  setUpTest(fail_close_with_custom_status_code_config_);
  InSequence s;

  EXPECT_CALL(route_rate_limit_, populateDescriptors(_, _, _, _))
      .WillOnce(SetArgReferee<0>(descriptor_));
  EXPECT_CALL(*client_, limit(_, _, _, _, _, 0))
      .WillOnce(
          WithArgs<0>(Invoke([&](Filters::Common::RateLimit::RequestCallbacks& callbacks) -> void {
            request_callbacks_ = &callbacks;
          })));

  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(request_headers_, false));

  EXPECT_CALL(filter_callbacks_.stream_info_,
              setResponseFlag(StreamInfo::CoreResponseFlag::RateLimitServiceError));

  EXPECT_CALL(filter_callbacks_, encodeHeaders_(_, true))
      .WillOnce(Invoke([&](const Http::ResponseHeaderMap& headers, bool) -> void {
        EXPECT_EQ(headers.getStatusValue(),
                  std::to_string(enumToInt(Http::Code::ServiceUnavailable)));
      }));

  request_callbacks_->complete(Filters::Common::RateLimit::LimitStatus::Error, nullptr, nullptr,
                               nullptr, "", nullptr);

  EXPECT_EQ(
      1U,
      filter_callbacks_.clusterInfo()->statsScope().counterFromStatName(ratelimit_error_).value());
  EXPECT_EQ(0U, filter_callbacks_.clusterInfo()
                    ->statsScope()
                    .counterFromStatName(ratelimit_failure_mode_allowed_)
                    .value());
  EXPECT_EQ("rate_limiter_error", filter_callbacks_.details());
}

TEST_F(HttpRateLimitFilterTest, LimitResponse) {
  setUpTest(filter_config_);
  InSequence s;

  EXPECT_CALL(route_rate_limit_, populateDescriptors(_, _, _, _))
      .WillOnce(SetArgReferee<0>(descriptor_));
  EXPECT_CALL(*client_, limit(_, _, _, _, _, 0))
      .WillOnce(
          WithArgs<0>(Invoke([&](Filters::Common::RateLimit::RequestCallbacks& callbacks) -> void {
            request_callbacks_ = &callbacks;
          })));

  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(request_headers_, false));

  EXPECT_CALL(filter_callbacks_.stream_info_,
              setResponseFlag(StreamInfo::CoreResponseFlag::RateLimited));

  Http::ResponseHeaderMapPtr h{new Http::TestResponseHeaderMapImpl()};
  Http::TestResponseHeaderMapImpl response_headers{
      {":status", "429"},
      {"x-envoy-ratelimited", Http::Headers::get().EnvoyRateLimitedValues.True}};
  EXPECT_CALL(filter_callbacks_, encodeHeaders_(HeaderMapEqualRef(&response_headers), true));
  EXPECT_CALL(filter_callbacks_, continueDecoding()).Times(0);

  request_callbacks_->complete(Filters::Common::RateLimit::LimitStatus::OverLimit, nullptr,
                               std::move(h), nullptr, "", nullptr);

  EXPECT_EQ(1U, filter_callbacks_.clusterInfo()
                    ->statsScope()
                    .counterFromStatName(ratelimit_over_limit_)
                    .value());
  EXPECT_EQ(
      1U,
      filter_callbacks_.clusterInfo()->statsScope().counterFromStatName(upstream_rq_4xx_).value());
  EXPECT_EQ(
      1U,
      filter_callbacks_.clusterInfo()->statsScope().counterFromStatName(upstream_rq_429_).value());
  EXPECT_EQ("request_rate_limited", filter_callbacks_.details());
}

TEST_F(HttpRateLimitFilterTest, LimitResponseWithDynamicMetadata) {
  setUpTest(filter_config_);
  InSequence s;

  EXPECT_CALL(route_rate_limit_, populateDescriptors(_, _, _, _))
      .WillOnce(SetArgReferee<0>(descriptor_));
  EXPECT_CALL(*client_, limit(_, _, _, _, _, 0))
      .WillOnce(
          WithArgs<0>(Invoke([&](Filters::Common::RateLimit::RequestCallbacks& callbacks) -> void {
            request_callbacks_ = &callbacks;
          })));

  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(request_headers_, false));

  Filters::Common::RateLimit::DynamicMetadataPtr dynamic_metadata =
      std::make_unique<ProtobufWkt::Struct>();
  auto* fields = dynamic_metadata->mutable_fields();
  (*fields)["name"] = ValueUtil::stringValue("my-limit");
  (*fields)["x"] = ValueUtil::numberValue(3);
  EXPECT_CALL(filter_callbacks_.stream_info_, setDynamicMetadata(_, _))
      .WillOnce(Invoke([&dynamic_metadata](const std::string& ns,
                                           const ProtobufWkt::Struct& returned_dynamic_metadata) {
        EXPECT_EQ(ns, "envoy.filters.http.ratelimit");
        EXPECT_TRUE(TestUtility::protoEqual(returned_dynamic_metadata, *dynamic_metadata));
      }));

  EXPECT_CALL(filter_callbacks_.stream_info_,
              setResponseFlag(StreamInfo::CoreResponseFlag::RateLimited));

  Http::ResponseHeaderMapPtr h{new Http::TestResponseHeaderMapImpl()};
  Http::TestResponseHeaderMapImpl response_headers{
      {":status", "429"},
      {"x-envoy-ratelimited", Http::Headers::get().EnvoyRateLimitedValues.True}};
  EXPECT_CALL(filter_callbacks_, encodeHeaders_(HeaderMapEqualRef(&response_headers), true));
  EXPECT_CALL(filter_callbacks_, continueDecoding()).Times(0);

  request_callbacks_->complete(Filters::Common::RateLimit::LimitStatus::OverLimit, nullptr,
                               std::move(h), nullptr, "", std::move(dynamic_metadata));

  EXPECT_EQ(1U, filter_callbacks_.clusterInfo()
                    ->statsScope()
                    .counterFromStatName(ratelimit_over_limit_)
                    .value());
  EXPECT_EQ(
      1U,
      filter_callbacks_.clusterInfo()->statsScope().counterFromStatName(upstream_rq_4xx_).value());
  EXPECT_EQ(
      1U,
      filter_callbacks_.clusterInfo()->statsScope().counterFromStatName(upstream_rq_429_).value());
  EXPECT_EQ("request_rate_limited", filter_callbacks_.details());
}

TEST_F(HttpRateLimitFilterTest, LimitResponseWithHeaders) {
  setUpTest(filter_config_);
  InSequence s;

  EXPECT_CALL(route_rate_limit_, populateDescriptors(_, _, _, _))
      .WillOnce(SetArgReferee<0>(descriptor_));
  EXPECT_CALL(*client_, limit(_, _, _, _, _, 0))
      .WillOnce(
          WithArgs<0>(Invoke([&](Filters::Common::RateLimit::RequestCallbacks& callbacks) -> void {
            request_callbacks_ = &callbacks;
          })));

  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(request_headers_, false));
  EXPECT_EQ(Http::Filter1xxHeadersStatus::Continue, filter_->encode1xxHeaders(response_headers_));
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers_, false));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(response_data_, false));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->encodeTrailers(response_trailers_));

  EXPECT_CALL(filter_callbacks_.stream_info_,
              setResponseFlag(StreamInfo::CoreResponseFlag::RateLimited));

  Http::HeaderMapPtr rl_headers{new Http::TestResponseHeaderMapImpl{
      {"x-ratelimit-limit", "1000"}, {"x-ratelimit-remaining", "0"}, {"retry-after", "33"}}};
  Http::TestResponseHeaderMapImpl expected_headers(*rl_headers);
  expected_headers.addCopy(":status", "429");
  expected_headers.addCopy("x-envoy-ratelimited", Http::Headers::get().EnvoyRateLimitedValues.True);

  EXPECT_CALL(filter_callbacks_, encodeHeaders_(HeaderMapEqualRef(&expected_headers), true));
  EXPECT_CALL(filter_callbacks_, continueDecoding()).Times(0);

  Http::HeaderMapPtr request_headers_to_add{
      new Http::TestRequestHeaderMapImpl{{"x-rls-rate-limited", "true"}}};

  Http::ResponseHeaderMapPtr h{new Http::TestResponseHeaderMapImpl(*rl_headers)};
  Http::RequestHeaderMapPtr uh{new Http::TestRequestHeaderMapImpl(*request_headers_to_add)};
  request_callbacks_->complete(Filters::Common::RateLimit::LimitStatus::OverLimit, nullptr,
                               std::move(h), std::move(uh), "", nullptr);

  EXPECT_THAT(*request_headers_to_add, Not(IsSubsetOfHeaders(request_headers_)));
  EXPECT_EQ(1U, filter_callbacks_.clusterInfo()
                    ->statsScope()
                    .counterFromStatName(ratelimit_over_limit_)
                    .value());
  EXPECT_EQ(
      1U,
      filter_callbacks_.clusterInfo()->statsScope().counterFromStatName(upstream_rq_4xx_).value());
  EXPECT_EQ(
      1U,
      filter_callbacks_.clusterInfo()->statsScope().counterFromStatName(upstream_rq_429_).value());
}

TEST_F(HttpRateLimitFilterTest, LimitResponseWithBody) {
  setUpTest(filter_config_);
  InSequence s;

  EXPECT_CALL(route_rate_limit_, populateDescriptors(_, _, _, _))
      .WillOnce(SetArgReferee<0>(descriptor_));
  EXPECT_CALL(*client_, limit(_, _, _, _, _, 0))
      .WillOnce(
          WithArgs<0>(Invoke([&](Filters::Common::RateLimit::RequestCallbacks& callbacks) -> void {
            request_callbacks_ = &callbacks;
          })));

  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(request_headers_, false));
  EXPECT_EQ(Http::Filter1xxHeadersStatus::Continue, filter_->encode1xxHeaders(response_headers_));
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers_, false));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(response_data_, false));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->encodeTrailers(response_trailers_));

  EXPECT_CALL(filter_callbacks_.stream_info_,
              setResponseFlag(StreamInfo::CoreResponseFlag::RateLimited));

  const std::string response_body = "this is a custom over limit response body.";
  const std::string content_length = std::to_string(response_body.length());
  Http::HeaderMapPtr rl_headers{new Http::TestResponseHeaderMapImpl{
      {"x-ratelimit-limit", "1000"}, {"x-ratelimit-remaining", "0"}, {"retry-after", "33"}}};
  Http::TestResponseHeaderMapImpl expected_headers{};
  // We construct the expected_headers map in careful order, because HeaderMapEqualRef below
  // compares two header maps in order. In practice, content-length and content-type headers
  // are added before additional ratelimit headers and the final x-envoy-ratelimited header.
  expected_headers.addCopy(":status", "429");
  expected_headers.addCopy("content-length", std::string(content_length));
  expected_headers.addCopy("content-type", "text/plain");
  expected_headers.copyFrom(*rl_headers);
  expected_headers.addCopy("x-envoy-ratelimited", Http::Headers::get().EnvoyRateLimitedValues.True);

  EXPECT_CALL(filter_callbacks_, encodeHeaders_(HeaderMapEqualRef(&expected_headers), false));
  EXPECT_CALL(filter_callbacks_, continueDecoding()).Times(0);
  EXPECT_CALL(filter_callbacks_, encodeData(_, true))
      .WillOnce(
          Invoke([&](Buffer::Instance& data, bool) { EXPECT_EQ(data.toString(), response_body); }));

  Http::HeaderMapPtr request_headers_to_add{
      new Http::TestRequestHeaderMapImpl{{"x-rls-rate-limited", "true"}}};

  Http::ResponseHeaderMapPtr h{new Http::TestResponseHeaderMapImpl(*rl_headers)};
  Http::RequestHeaderMapPtr uh{new Http::TestRequestHeaderMapImpl(*request_headers_to_add)};
  request_callbacks_->complete(Filters::Common::RateLimit::LimitStatus::OverLimit, nullptr,
                               std::move(h), std::move(uh), response_body, nullptr);

  EXPECT_THAT(*request_headers_to_add, Not(IsSubsetOfHeaders(request_headers_)));
  EXPECT_EQ(1U, filter_callbacks_.clusterInfo()
                    ->statsScope()
                    .counterFromStatName(ratelimit_over_limit_)
                    .value());
  EXPECT_EQ(
      1U,
      filter_callbacks_.clusterInfo()->statsScope().counterFromStatName(upstream_rq_4xx_).value());
  EXPECT_EQ(
      1U,
      filter_callbacks_.clusterInfo()->statsScope().counterFromStatName(upstream_rq_429_).value());
}

TEST_F(HttpRateLimitFilterTest, LimitResponseWithBodyAndContentType) {
  setUpTest(filter_config_);
  InSequence s;

  EXPECT_CALL(route_rate_limit_, populateDescriptors(_, _, _, _))
      .WillOnce(SetArgReferee<0>(descriptor_));
  EXPECT_CALL(*client_, limit(_, _, _, _, _, 0))
      .WillOnce(
          WithArgs<0>(Invoke([&](Filters::Common::RateLimit::RequestCallbacks& callbacks) -> void {
            request_callbacks_ = &callbacks;
          })));

  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(request_headers_, false));
  EXPECT_EQ(Http::Filter1xxHeadersStatus::Continue, filter_->encode1xxHeaders(response_headers_));
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers_, false));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(response_data_, false));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->encodeTrailers(response_trailers_));

  EXPECT_CALL(filter_callbacks_.stream_info_,
              setResponseFlag(StreamInfo::CoreResponseFlag::RateLimited));

  const std::string response_body = R"EOF(
  { "message": "this is a custom over limit response body as json.", "retry-after": "33" }
  )EOF";
  const std::string content_length = std::to_string(response_body.length());
  Http::HeaderMapPtr rl_headers{
      new Http::TestResponseHeaderMapImpl{{"content-type", "application/json"},
                                          {"x-ratelimit-limit", "1000"},
                                          {"x-ratelimit-remaining", "0"},
                                          {"retry-after", "33"}}};
  Http::TestResponseHeaderMapImpl expected_headers{};
  // We construct the expected_headers map in careful order, because HeaderMapEqualRef below
  // compares two header maps in order. In practice, content-length and content-type headers
  // are added before additional ratelimit headers and the final x-envoy-ratelimited header.
  // Additionally, we skip explicitly adding content-type here because it's already part of
  // `rl_headers` above.
  expected_headers.addCopy(":status", "429");
  expected_headers.addCopy("content-length", std::string(content_length));
  expected_headers.copyFrom(*rl_headers);
  expected_headers.addCopy("x-envoy-ratelimited", Http::Headers::get().EnvoyRateLimitedValues.True);

  EXPECT_CALL(filter_callbacks_, encodeHeaders_(HeaderMapEqualRef(&expected_headers), false));
  EXPECT_CALL(filter_callbacks_, continueDecoding()).Times(0);
  EXPECT_CALL(filter_callbacks_, encodeData(_, true))
      .WillOnce(
          Invoke([&](Buffer::Instance& data, bool) { EXPECT_EQ(data.toString(), response_body); }));

  Http::HeaderMapPtr request_headers_to_add{
      new Http::TestRequestHeaderMapImpl{{"x-rls-rate-limited", "true"}}};

  Http::ResponseHeaderMapPtr h{new Http::TestResponseHeaderMapImpl(*rl_headers)};
  Http::RequestHeaderMapPtr uh{new Http::TestRequestHeaderMapImpl(*request_headers_to_add)};
  request_callbacks_->complete(Filters::Common::RateLimit::LimitStatus::OverLimit, nullptr,
                               std::move(h), std::move(uh), response_body, nullptr);

  EXPECT_THAT(*request_headers_to_add, Not(IsSubsetOfHeaders(request_headers_)));
  EXPECT_EQ(1U, filter_callbacks_.clusterInfo()
                    ->statsScope()
                    .counterFromStatName(ratelimit_over_limit_)
                    .value());
  EXPECT_EQ(
      1U,
      filter_callbacks_.clusterInfo()->statsScope().counterFromStatName(upstream_rq_4xx_).value());
  EXPECT_EQ(
      1U,
      filter_callbacks_.clusterInfo()->statsScope().counterFromStatName(upstream_rq_429_).value());
}

TEST_F(HttpRateLimitFilterTest, LimitResponseWithFilterHeaders) {
  setUpTest(enable_x_ratelimit_headers_config_);
  InSequence s;

  EXPECT_CALL(route_rate_limit_, populateDescriptors(_, _, _, _))
      .WillOnce(SetArgReferee<0>(descriptor_));
  EXPECT_CALL(*client_, limit(_, _, _, _, _, 0))
      .WillOnce(
          WithArgs<0>(Invoke([&](Filters::Common::RateLimit::RequestCallbacks& callbacks) -> void {
            request_callbacks_ = &callbacks;
          })));

  EXPECT_CALL(filter_callbacks_.stream_info_,
              setResponseFlag(StreamInfo::CoreResponseFlag::RateLimited));

  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(request_headers_, false));
  EXPECT_EQ(Http::Filter1xxHeadersStatus::Continue, filter_->encode1xxHeaders(response_headers_));
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers_, false));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(response_data_, false));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->encodeTrailers(response_trailers_));

  Http::TestResponseHeaderMapImpl expected_headers{
      {":status", "429"},
      {"x-envoy-ratelimited", Http::Headers::get().EnvoyRateLimitedValues.True},
      {"x-ratelimit-limit", "1, 1;w=60;name=\"first\", 4;w=3600;name=\"second\""},
      {"x-ratelimit-remaining", "2"},
      {"x-ratelimit-reset", "3"}};
  EXPECT_CALL(filter_callbacks_, encodeHeaders_(HeaderMapEqualRef(&expected_headers), true));
  EXPECT_CALL(filter_callbacks_, continueDecoding()).Times(0);

  auto descriptor_statuses = {
      Envoy::RateLimit::buildDescriptorStatus(
          1, envoy::service::ratelimit::v3::RateLimitResponse::RateLimit::MINUTE, "first", 2, 3),
      Envoy::RateLimit::buildDescriptorStatus(
          4, envoy::service::ratelimit::v3::RateLimitResponse::RateLimit::HOUR, "second", 5, 6)};
  auto descriptor_statuses_ptr =
      std::make_unique<Filters::Common::RateLimit::DescriptorStatusList>(descriptor_statuses);
  request_callbacks_->complete(Filters::Common::RateLimit::LimitStatus::OverLimit,
                               std::move(descriptor_statuses_ptr), nullptr, nullptr, "", nullptr);
  EXPECT_EQ(1U, filter_callbacks_.clusterInfo()
                    ->statsScope()
                    .counterFromStatName(ratelimit_over_limit_)
                    .value());
  EXPECT_EQ(
      1U,
      filter_callbacks_.clusterInfo()->statsScope().counterFromStatName(upstream_rq_4xx_).value());
  EXPECT_EQ(
      1U,
      filter_callbacks_.clusterInfo()->statsScope().counterFromStatName(upstream_rq_429_).value());
}

TEST_F(HttpRateLimitFilterTest, LimitResponseWithoutEnvoyRateLimitedHeader) {
  setUpTest(disable_x_envoy_ratelimited_header_config_);
  InSequence s;

  EXPECT_CALL(route_rate_limit_, populateDescriptors(_, _, _, _))
      .WillOnce(SetArgReferee<0>(descriptor_));
  EXPECT_CALL(*client_, limit(_, _, _, _, _, 0))
      .WillOnce(
          WithArgs<0>(Invoke([&](Filters::Common::RateLimit::RequestCallbacks& callbacks) -> void {
            request_callbacks_ = &callbacks;
          })));

  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(request_headers_, false));

  EXPECT_CALL(filter_callbacks_.stream_info_,
              setResponseFlag(StreamInfo::CoreResponseFlag::RateLimited));

  Http::ResponseHeaderMapPtr h{new Http::TestResponseHeaderMapImpl()};
  Http::TestResponseHeaderMapImpl response_headers{{":status", "429"}};
  EXPECT_CALL(filter_callbacks_, encodeHeaders_(HeaderMapEqualRef(&response_headers), true));
  EXPECT_CALL(filter_callbacks_, continueDecoding()).Times(0);

  request_callbacks_->complete(Filters::Common::RateLimit::LimitStatus::OverLimit, nullptr,
                               std::move(h), nullptr, "", nullptr);

  EXPECT_EQ(1U, filter_callbacks_.clusterInfo()
                    ->statsScope()
                    .counterFromStatName(ratelimit_over_limit_)
                    .value());
  EXPECT_EQ(
      1U,
      filter_callbacks_.clusterInfo()->statsScope().counterFromStatName(upstream_rq_4xx_).value());
  EXPECT_EQ(
      1U,
      filter_callbacks_.clusterInfo()->statsScope().counterFromStatName(upstream_rq_429_).value());
  EXPECT_EQ("request_rate_limited", filter_callbacks_.details());
}

TEST_F(HttpRateLimitFilterTest, LimitResponseRuntimeDisabled) {
  setUpTest(filter_config_);
  InSequence s;

  EXPECT_CALL(route_rate_limit_, populateDescriptors(_, _, _, _))
      .WillOnce(SetArgReferee<0>(descriptor_));
  EXPECT_CALL(*client_, limit(_, _, _, _, _, 0))
      .WillOnce(
          WithArgs<0>(Invoke([&](Filters::Common::RateLimit::RequestCallbacks& callbacks) -> void {
            request_callbacks_ = &callbacks;
          })));

  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(request_headers_, false));

  EXPECT_CALL(factory_context_.runtime_loader_.snapshot_,
              featureEnabled("ratelimit.http_filter_enforcing", 100))
      .WillOnce(Return(false));
  EXPECT_CALL(filter_callbacks_, continueDecoding());
  Http::ResponseHeaderMapPtr h{new Http::TestResponseHeaderMapImpl()};
  request_callbacks_->complete(Filters::Common::RateLimit::LimitStatus::OverLimit, nullptr,
                               std::move(h), nullptr, "", nullptr);

  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->decodeData(data_, false));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->decodeTrailers(request_trailers_));
  EXPECT_EQ(Http::Filter1xxHeadersStatus::Continue, filter_->encode1xxHeaders(response_headers_));
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers_, false));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(response_data_, false));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->encodeTrailers(response_trailers_));

  EXPECT_EQ(1U, filter_callbacks_.clusterInfo()
                    ->statsScope()
                    .counterFromStatName(ratelimit_over_limit_)
                    .value());
  EXPECT_EQ(
      1U,
      filter_callbacks_.clusterInfo()->statsScope().counterFromStatName(upstream_rq_4xx_).value());
  EXPECT_EQ(
      1U,
      filter_callbacks_.clusterInfo()->statsScope().counterFromStatName(upstream_rq_429_).value());
}

TEST_F(HttpRateLimitFilterTest, LimitResponseRuntimeDisabledFromFilterConfig) {
  setUpTest(filter_config_with_filter_enforced_);
  InSequence s;

  EXPECT_CALL(route_rate_limit_, populateDescriptors(_, _, _, _))
      .WillOnce(SetArgReferee<0>(descriptor_));
  EXPECT_CALL(*client_, limit(_, _, _, _, _, 0))
      .WillOnce(
          WithArgs<0>(Invoke([&](Filters::Common::RateLimit::RequestCallbacks& callbacks) -> void {
            request_callbacks_ = &callbacks;
          })));

  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(request_headers_, false));

  EXPECT_CALL(
      factory_context_.runtime_loader_.snapshot_,
      featureEnabled(absl::string_view("test_enforced"),
                     testing::Matcher<const envoy::type::v3::FractionalPercent&>(Percent(50))))
      .WillOnce(testing::Return(false));

  // Explicit configuration in the filter config should override the default runtime key.
  EXPECT_CALL(factory_context_.runtime_loader_.snapshot_,
              featureEnabled("ratelimit.http_filter_enforcing", 100))
      .Times(0);

  EXPECT_CALL(filter_callbacks_, continueDecoding());
  Http::ResponseHeaderMapPtr h{new Http::TestResponseHeaderMapImpl()};
  request_callbacks_->complete(Filters::Common::RateLimit::LimitStatus::OverLimit, nullptr,
                               std::move(h), nullptr, "", nullptr);

  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->decodeData(data_, false));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->decodeTrailers(request_trailers_));
  EXPECT_EQ(Http::Filter1xxHeadersStatus::Continue, filter_->encode1xxHeaders(response_headers_));
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers_, false));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(response_data_, false));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->encodeTrailers(response_trailers_));

  EXPECT_EQ(1U, filter_callbacks_.clusterInfo()
                    ->statsScope()
                    .counterFromStatName(ratelimit_over_limit_)
                    .value());
  EXPECT_EQ(
      1U,
      filter_callbacks_.clusterInfo()->statsScope().counterFromStatName(upstream_rq_4xx_).value());
  EXPECT_EQ(
      1U,
      filter_callbacks_.clusterInfo()->statsScope().counterFromStatName(upstream_rq_429_).value());
}

TEST_F(HttpRateLimitFilterTest, LimitResponseWithRateLimitedStatus) {
  setUpTest(rate_limited_status_config_);
  InSequence s;

  EXPECT_CALL(route_rate_limit_, populateDescriptors(_, _, _, _))
      .WillOnce(SetArgReferee<0>(descriptor_));
  EXPECT_CALL(*client_, limit(_, _, _, _, _, 0))
      .WillOnce(
          WithArgs<0>(Invoke([&](Filters::Common::RateLimit::RequestCallbacks& callbacks) -> void {
            request_callbacks_ = &callbacks;
          })));

  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(request_headers_, false));

  EXPECT_CALL(filter_callbacks_.stream_info_,
              setResponseFlag(StreamInfo::CoreResponseFlag::RateLimited));

  Http::ResponseHeaderMapPtr h{new Http::TestResponseHeaderMapImpl()};
  Http::TestResponseHeaderMapImpl response_headers{
      {":status", "503"},
      {"x-envoy-ratelimited", Http::Headers::get().EnvoyRateLimitedValues.True}};
  EXPECT_CALL(filter_callbacks_, encodeHeaders_(HeaderMapEqualRef(&response_headers), true));
  EXPECT_CALL(filter_callbacks_, continueDecoding()).Times(0);

  request_callbacks_->complete(Filters::Common::RateLimit::LimitStatus::OverLimit, nullptr,
                               std::move(h), nullptr, "", nullptr);

  EXPECT_EQ(1U, filter_callbacks_.clusterInfo()
                    ->statsScope()
                    .counterFromStatName(ratelimit_over_limit_)
                    .value());
  EXPECT_EQ(
      1U,
      filter_callbacks_.clusterInfo()->statsScope().counterFromStatName(upstream_rq_5xx_).value());
  EXPECT_EQ(
      1U,
      filter_callbacks_.clusterInfo()->statsScope().counterFromStatName(upstream_rq_503_).value());
  EXPECT_EQ("request_rate_limited", filter_callbacks_.details());
}

TEST_F(HttpRateLimitFilterTest, LimitResponseWithInvalidRateLimitedStatus) {
  setUpTest(invalid_rate_limited_status_config_);
  InSequence s;

  EXPECT_CALL(route_rate_limit_, populateDescriptors(_, _, _, _))
      .WillOnce(SetArgReferee<0>(descriptor_));
  EXPECT_CALL(*client_, limit(_, _, _, _, _, 0))
      .WillOnce(
          WithArgs<0>(Invoke([&](Filters::Common::RateLimit::RequestCallbacks& callbacks) -> void {
            request_callbacks_ = &callbacks;
          })));

  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(request_headers_, false));

  EXPECT_CALL(filter_callbacks_.stream_info_,
              setResponseFlag(StreamInfo::CoreResponseFlag::RateLimited));

  Http::ResponseHeaderMapPtr h{new Http::TestResponseHeaderMapImpl()};
  Http::TestResponseHeaderMapImpl response_headers{
      {":status", "429"},
      {"x-envoy-ratelimited", Http::Headers::get().EnvoyRateLimitedValues.True}};
  EXPECT_CALL(filter_callbacks_, encodeHeaders_(HeaderMapEqualRef(&response_headers), true));
  EXPECT_CALL(filter_callbacks_, continueDecoding()).Times(0);

  request_callbacks_->complete(Filters::Common::RateLimit::LimitStatus::OverLimit, nullptr,
                               std::move(h), nullptr, "", nullptr);

  EXPECT_EQ(1U, filter_callbacks_.clusterInfo()
                    ->statsScope()
                    .counterFromStatName(ratelimit_over_limit_)
                    .value());
  EXPECT_EQ(
      1U,
      filter_callbacks_.clusterInfo()->statsScope().counterFromStatName(upstream_rq_4xx_).value());
  EXPECT_EQ(
      1U,
      filter_callbacks_.clusterInfo()->statsScope().counterFromStatName(upstream_rq_429_).value());
  EXPECT_EQ("request_rate_limited", filter_callbacks_.details());
}

TEST_F(HttpRateLimitFilterTest, ResetDuringCall) {
  setUpTest(filter_config_);
  InSequence s;

  EXPECT_CALL(route_rate_limit_, populateDescriptors(_, _, _, _))
      .WillOnce(SetArgReferee<0>(descriptor_));
  EXPECT_CALL(*client_, limit(_, _, _, _, _, 0))
      .WillOnce(
          WithArgs<0>(Invoke([&](Filters::Common::RateLimit::RequestCallbacks& callbacks) -> void {
            request_callbacks_ = &callbacks;
          })));

  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(request_headers_, false));

  EXPECT_CALL(*client_, cancel());
  filter_->onDestroy();
}

TEST_F(HttpRateLimitFilterTest, RouteRateLimitDisabledForRouteKey) {
  route_rate_limit_.disable_key_ = "test_key";
  setUpTest(filter_config_);

  ON_CALL(factory_context_.runtime_loader_.snapshot_,
          featureEnabled("ratelimit.test_key.http_filter_enabled", 100))
      .WillByDefault(Return(false));

  EXPECT_CALL(route_rate_limit_, populateDescriptors(_, _, _, _)).Times(0);
  EXPECT_CALL(*client_, limit(_, _, _, _, _, 0)).Times(0);

  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers_, false));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->decodeData(data_, false));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->decodeTrailers(request_trailers_));
  EXPECT_EQ(Http::Filter1xxHeadersStatus::Continue, filter_->encode1xxHeaders(response_headers_));
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers_, false));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(response_data_, false));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->encodeTrailers(response_trailers_));
}

TEST_F(HttpRateLimitFilterTest, VirtualHostRateLimitDisabledForRouteKey) {
  vh_rate_limit_.disable_key_ = "test_vh_key";
  setUpTest(filter_config_);

  ON_CALL(factory_context_.runtime_loader_.snapshot_,
          featureEnabled("ratelimit.test_vh_key.http_filter_enabled", 100))
      .WillByDefault(Return(false));

  EXPECT_CALL(vh_rate_limit_, populateDescriptors(_, _, _, _)).Times(0);
  EXPECT_CALL(*client_, limit(_, _, _, _, _, 0)).Times(0);

  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers_, false));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->decodeData(data_, false));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->decodeTrailers(request_trailers_));
  EXPECT_EQ(Http::Filter1xxHeadersStatus::Continue, filter_->encode1xxHeaders(response_headers_));
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers_, false));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(response_data_, false));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->encodeTrailers(response_trailers_));
}

TEST_F(HttpRateLimitFilterTest, IncorrectRequestType) {
  std::string internal_filter_config = R"EOF(
  {
    "domain": "foo",
    "request_type" : "internal"
  }
  )EOF";
  setUpTest(internal_filter_config);

  EXPECT_CALL(route_rate_limit_, populateDescriptors(_, _, _, _)).Times(0);
  EXPECT_CALL(vh_rate_limit_, populateDescriptors(_, _, _, _)).Times(0);
  EXPECT_CALL(*client_, limit(_, _, _, _, _, 0)).Times(0);
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers_, false));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->decodeData(data_, false));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->decodeTrailers(request_trailers_));
  EXPECT_EQ(Http::Filter1xxHeadersStatus::Continue, filter_->encode1xxHeaders(response_headers_));
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers_, false));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(response_data_, false));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->encodeTrailers(response_trailers_));

  std::string external_filter_config = R"EOF(
  {
    "domain": "foo",
    "request_type" : "external"
  }
  )EOF";
  setUpTest(external_filter_config);
  Http::TestRequestHeaderMapImpl request_headers{{"x-envoy-internal", "true"}};

  EXPECT_CALL(route_rate_limit_, populateDescriptors(_, _, _, _)).Times(0);
  EXPECT_CALL(vh_rate_limit_, populateDescriptors(_, _, _, _)).Times(0);
  EXPECT_CALL(*client_, limit(_, _, _, _, _, 0)).Times(0);
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, false));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->decodeData(data_, false));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->decodeTrailers(request_trailers_));
  EXPECT_EQ(Http::Filter1xxHeadersStatus::Continue, filter_->encode1xxHeaders(response_headers_));
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers_, false));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(response_data_, false));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->encodeTrailers(response_trailers_));
}

TEST_F(HttpRateLimitFilterTest, InternalRequestType) {
  std::string internal_filter_config = R"EOF(
  {
    "domain": "foo",
    "request_type" : "internal"
  }
  )EOF";
  setUpTest(internal_filter_config);
  Http::TestRequestHeaderMapImpl request_headers{{"x-envoy-internal", "true"}};
  InSequence s;

  EXPECT_CALL(filter_callbacks_.route_->route_entry_.rate_limit_policy_, getApplicableRateLimit(0));

  EXPECT_CALL(route_rate_limit_, populateDescriptors(_, _, _, _))
      .WillOnce(SetArgReferee<0>(descriptor_));

  EXPECT_CALL(filter_callbacks_.route_->virtual_host_->rate_limit_policy_,
              getApplicableRateLimit(0));

  EXPECT_CALL(*client_, limit(_, "foo",
                              testing::ContainerEq(std::vector<RateLimit::Descriptor>{
                                  {{{"descriptor_key", "descriptor_value"}}}}),
                              _, _, 0))
      .WillOnce(
          WithArgs<0>(Invoke([&](Filters::Common::RateLimit::RequestCallbacks& callbacks) -> void {
            callbacks.complete(Filters::Common::RateLimit::LimitStatus::OK, nullptr, nullptr,
                               nullptr, "", nullptr);
          })));

  EXPECT_CALL(filter_callbacks_, continueDecoding()).Times(0);
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, false));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->decodeData(data_, false));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->decodeTrailers(request_trailers_));
  EXPECT_EQ(Http::Filter1xxHeadersStatus::Continue, filter_->encode1xxHeaders(response_headers_));
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers_, false));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(response_data_, false));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->encodeTrailers(response_trailers_));

  EXPECT_EQ(
      1U, filter_callbacks_.clusterInfo()->statsScope().counterFromStatName(ratelimit_ok_).value());
}

TEST_F(HttpRateLimitFilterTest, ExternalRequestType) {

  std::string external_filter_config = R"EOF(
  {
    "domain": "foo",
    "request_type" : "external"
  }
  )EOF";
  setUpTest(external_filter_config);
  Http::TestRequestHeaderMapImpl request_headers{{"x-envoy-internal", "false"}};
  InSequence s;

  EXPECT_CALL(filter_callbacks_.route_->route_entry_.rate_limit_policy_, getApplicableRateLimit(0));

  EXPECT_CALL(route_rate_limit_, populateDescriptors(_, _, _, _))
      .WillOnce(SetArgReferee<0>(descriptor_));

  EXPECT_CALL(filter_callbacks_.route_->virtual_host_->rate_limit_policy_,
              getApplicableRateLimit(0));

  EXPECT_CALL(*client_, limit(_, "foo",
                              testing::ContainerEq(std::vector<RateLimit::Descriptor>{
                                  {{{"descriptor_key", "descriptor_value"}}}}),
                              _, _, 0))
      .WillOnce(
          WithArgs<0>(Invoke([&](Filters::Common::RateLimit::RequestCallbacks& callbacks) -> void {
            callbacks.complete(Filters::Common::RateLimit::LimitStatus::OK, nullptr, nullptr,
                               nullptr, "", nullptr);
          })));

  EXPECT_CALL(filter_callbacks_, continueDecoding()).Times(0);
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, false));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->decodeData(data_, false));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->decodeTrailers(request_trailers_));
  EXPECT_EQ(Http::Filter1xxHeadersStatus::Continue, filter_->encode1xxHeaders(response_headers_));
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers_, false));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(response_data_, false));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->encodeTrailers(response_trailers_));

  EXPECT_EQ(
      1U, filter_callbacks_.clusterInfo()->statsScope().counterFromStatName(ratelimit_ok_).value());
}

TEST_F(HttpRateLimitFilterTest, DEPRECATED_FEATURE_TEST(ExcludeVirtualHost)) {
  std::string external_filter_config = R"EOF(
  {
    "domain": "foo"
  }
  )EOF";
  setUpTest(external_filter_config);
  envoy::extensions::filters::http::ratelimit::v3::RateLimitPerRoute settings;
  settings.clear_vh_rate_limits();
  absl::Status creation_status = absl::OkStatus();
  FilterConfigPerRoute per_route_config_(factory_context_, settings, creation_status);
  EXPECT_TRUE(creation_status.ok());

  EXPECT_CALL(filter_callbacks_.route_->route_entry_.rate_limit_policy_, getApplicableRateLimit(0));
  EXPECT_CALL(route_rate_limit_, populateDescriptors(_, _, _, _))
      .WillOnce(SetArgReferee<0>(descriptor_));

  EXPECT_CALL(filter_callbacks_.route_->route_entry_, includeVirtualHostRateLimits())
      .WillOnce(Return(false));

  EXPECT_CALL(*filter_callbacks_.route_, mostSpecificPerFilterConfig(_))
      .WillOnce(Return(&per_route_config_));

  EXPECT_CALL(filter_callbacks_.route_->route_entry_.rate_limit_policy_, empty())
      .WillOnce(Return(false));

  EXPECT_CALL(filter_callbacks_.route_->virtual_host_->rate_limit_policy_,
              getApplicableRateLimit(0))
      .Times(0);

  EXPECT_CALL(*client_, limit(_, "foo",
                              testing::ContainerEq(std::vector<RateLimit::Descriptor>{
                                  {{{"descriptor_key", "descriptor_value"}}}}),
                              _, _, 0))
      .WillOnce(
          WithArgs<0>(Invoke([&](Filters::Common::RateLimit::RequestCallbacks& callbacks) -> void {
            callbacks.complete(Filters::Common::RateLimit::LimitStatus::OK, nullptr, nullptr,
                               nullptr, "", nullptr);
          })));

  EXPECT_CALL(filter_callbacks_, continueDecoding()).Times(0);
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers_, false));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->decodeData(data_, false));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->decodeTrailers(request_trailers_));
  EXPECT_EQ(Http::Filter1xxHeadersStatus::Continue, filter_->encode1xxHeaders(response_headers_));
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers_, false));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(response_data_, false));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->encodeTrailers(response_trailers_));

  EXPECT_EQ(
      1U, filter_callbacks_.clusterInfo()->statsScope().counterFromStatName(ratelimit_ok_).value());
}

// Tests that the route rate limit is used when VhRateLimitsOptions::OVERRIDE and route rate limit
// is set
TEST_F(HttpRateLimitFilterTest, OverrideVHRateLimitOptionWithRouteRateLimitSet) {
  setUpTest(filter_config_);
  envoy::extensions::filters::http::ratelimit::v3::RateLimitPerRoute settings;
  settings.set_vh_rate_limits(
      envoy::extensions::filters::http::ratelimit::v3::RateLimitPerRoute::OVERRIDE);
  absl::Status creation_status = absl::OkStatus();
  FilterConfigPerRoute per_route_config_(factory_context_, settings, creation_status);
  EXPECT_TRUE(creation_status.ok());

  EXPECT_CALL(filter_callbacks_.route_->route_entry_.rate_limit_policy_, getApplicableRateLimit(0));
  EXPECT_CALL(route_rate_limit_, populateDescriptors(_, _, _, _))
      .WillOnce(SetArgReferee<0>(descriptor_));

  EXPECT_CALL(filter_callbacks_.route_->route_entry_, includeVirtualHostRateLimits())
      .WillOnce(Return(false));

  EXPECT_CALL(*filter_callbacks_.route_, mostSpecificPerFilterConfig(_))
      .WillOnce(Return(&per_route_config_));

  EXPECT_CALL(filter_callbacks_.route_->route_entry_.rate_limit_policy_, empty())
      .WillOnce(Return(false));

  EXPECT_CALL(filter_callbacks_.route_->virtual_host_->rate_limit_policy_,
              getApplicableRateLimit(0))
      .Times(0);

  EXPECT_CALL(*client_, limit(_, "foo",
                              testing::ContainerEq(std::vector<RateLimit::Descriptor>{
                                  {{{"descriptor_key", "descriptor_value"}}}}),
                              _, _, 0))
      .WillOnce(
          WithArgs<0>(Invoke([&](Filters::Common::RateLimit::RequestCallbacks& callbacks) -> void {
            callbacks.complete(Filters::Common::RateLimit::LimitStatus::OK, nullptr, nullptr,
                               nullptr, "", nullptr);
          })));

  EXPECT_CALL(filter_callbacks_, continueDecoding()).Times(0);
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers_, false));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->decodeData(data_, false));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->decodeTrailers(request_trailers_));
  EXPECT_EQ(Http::Filter1xxHeadersStatus::Continue, filter_->encode1xxHeaders(response_headers_));
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers_, false));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(response_data_, false));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->encodeTrailers(response_trailers_));

  EXPECT_EQ(
      1U, filter_callbacks_.clusterInfo()->statsScope().counterFromStatName(ratelimit_ok_).value());
}

// Tests that the virtual host rate limit is used when VhRateLimitsOptions::OVERRIDE is set and
// route rate limit is empty
TEST_F(HttpRateLimitFilterTest, OverrideVHRateLimitOptionWithoutRouteRateLimit) {
  setUpTest(filter_config_);
  envoy::extensions::filters::http::ratelimit::v3::RateLimitPerRoute settings;
  settings.set_vh_rate_limits(
      envoy::extensions::filters::http::ratelimit::v3::RateLimitPerRoute::OVERRIDE);
  absl::Status creation_status = absl::OkStatus();
  FilterConfigPerRoute per_route_config_(factory_context_, settings, creation_status);
  EXPECT_TRUE(creation_status.ok());

  EXPECT_CALL(filter_callbacks_.route_->route_entry_.rate_limit_policy_, getApplicableRateLimit(0));

  EXPECT_CALL(filter_callbacks_.route_->route_entry_, includeVirtualHostRateLimits())
      .WillOnce(Return(false));

  EXPECT_CALL(*filter_callbacks_.route_, mostSpecificPerFilterConfig(_))
      .WillOnce(Return(&per_route_config_));

  EXPECT_CALL(filter_callbacks_.route_->route_entry_.rate_limit_policy_, empty())
      .WillOnce(Return(true));

  EXPECT_CALL(filter_callbacks_.route_->virtual_host_->rate_limit_policy_,
              getApplicableRateLimit(0));

  EXPECT_CALL(vh_rate_limit_, populateDescriptors(_, _, _, _))
      .WillOnce(SetArgReferee<0>(descriptor_));

  EXPECT_CALL(*client_, limit(_, "foo",
                              testing::ContainerEq(std::vector<RateLimit::Descriptor>{
                                  {{{"descriptor_key", "descriptor_value"}}}}),
                              _, _, 0))
      .WillOnce(
          WithArgs<0>(Invoke([&](Filters::Common::RateLimit::RequestCallbacks& callbacks) -> void {
            callbacks.complete(Filters::Common::RateLimit::LimitStatus::OK, nullptr, nullptr,
                               nullptr, "", nullptr);
          })));

  EXPECT_CALL(filter_callbacks_, continueDecoding()).Times(0);
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers_, false));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->decodeData(data_, false));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->decodeTrailers(request_trailers_));
  EXPECT_EQ(Http::Filter1xxHeadersStatus::Continue, filter_->encode1xxHeaders(response_headers_));
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers_, false));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(response_data_, false));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->encodeTrailers(response_trailers_));

  EXPECT_EQ(
      1U, filter_callbacks_.clusterInfo()->statsScope().counterFromStatName(ratelimit_ok_).value());
}

// Tests that the virtual host rate limit is used when VhRateLimitsOptions::INCLUDE is set and route
// rate limit is empty
TEST_F(HttpRateLimitFilterTest, IncludeVHRateLimitOptionWithOnlyVHRateLimitSet) {
  setUpTest(filter_config_);
  envoy::extensions::filters::http::ratelimit::v3::RateLimitPerRoute settings;
  settings.set_vh_rate_limits(
      envoy::extensions::filters::http::ratelimit::v3::RateLimitPerRoute::INCLUDE);
  absl::Status creation_status = absl::OkStatus();
  FilterConfigPerRoute per_route_config_(factory_context_, settings, creation_status);
  EXPECT_TRUE(creation_status.ok());

  EXPECT_CALL(filter_callbacks_.route_->route_entry_.rate_limit_policy_, getApplicableRateLimit(0));

  EXPECT_CALL(filter_callbacks_.route_->route_entry_, includeVirtualHostRateLimits())
      .WillOnce(Return(false));

  EXPECT_CALL(*filter_callbacks_.route_, mostSpecificPerFilterConfig(_))
      .WillOnce(Return(&per_route_config_));

  EXPECT_CALL(filter_callbacks_.route_->virtual_host_->rate_limit_policy_,
              getApplicableRateLimit(0));

  EXPECT_CALL(vh_rate_limit_, populateDescriptors(_, _, _, _))
      .WillOnce(SetArgReferee<0>(descriptor_two_));

  EXPECT_CALL(*client_,
              limit(_, "foo",
                    testing::ContainerEq(std::vector<RateLimit::Descriptor>{{{{"key", "value"}}}}),
                    _, _, 0))
      .WillOnce(
          WithArgs<0>(Invoke([&](Filters::Common::RateLimit::RequestCallbacks& callbacks) -> void {
            callbacks.complete(Filters::Common::RateLimit::LimitStatus::OK, nullptr, nullptr,
                               nullptr, "", nullptr);
          })));

  EXPECT_CALL(filter_callbacks_, continueDecoding()).Times(0);
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers_, false));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->decodeData(data_, false));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->decodeTrailers(request_trailers_));
  EXPECT_EQ(Http::Filter1xxHeadersStatus::Continue, filter_->encode1xxHeaders(response_headers_));
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers_, false));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(response_data_, false));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->encodeTrailers(response_trailers_));

  EXPECT_EQ(
      1U, filter_callbacks_.clusterInfo()->statsScope().counterFromStatName(ratelimit_ok_).value());
}

// Tests that the virtual host rate limit is used when VhRateLimitsOptions::INCLUDE and route rate
// limit is set
TEST_F(HttpRateLimitFilterTest, IncludeVHRateLimitOptionWithRouteAndVHRateLimitSet) {
  setUpTest(filter_config_);
  envoy::extensions::filters::http::ratelimit::v3::RateLimitPerRoute settings;
  settings.set_vh_rate_limits(
      envoy::extensions::filters::http::ratelimit::v3::RateLimitPerRoute::INCLUDE);
  absl::Status creation_status = absl::OkStatus();
  FilterConfigPerRoute per_route_config_(factory_context_, settings, creation_status);
  EXPECT_TRUE(creation_status.ok());

  EXPECT_CALL(filter_callbacks_.route_->route_entry_.rate_limit_policy_, getApplicableRateLimit(0));
  EXPECT_CALL(route_rate_limit_, populateDescriptors(_, _, _, _))
      .WillOnce(SetArgReferee<0>(descriptor_));

  EXPECT_CALL(filter_callbacks_.route_->route_entry_, includeVirtualHostRateLimits())
      .WillOnce(Return(false));

  EXPECT_CALL(*filter_callbacks_.route_, mostSpecificPerFilterConfig(_))
      .WillOnce(Return(&per_route_config_));

  EXPECT_CALL(filter_callbacks_.route_->virtual_host_->rate_limit_policy_,
              getApplicableRateLimit(0));

  EXPECT_CALL(vh_rate_limit_, populateDescriptors(_, _, _, _))
      .WillOnce(SetArgReferee<0>(descriptor_two_));

  EXPECT_CALL(*client_,
              limit(_, "foo",
                    testing::ContainerEq(std::vector<RateLimit::Descriptor>{{{{"key", "value"}}}}),
                    _, _, 0))
      .WillOnce(
          WithArgs<0>(Invoke([&](Filters::Common::RateLimit::RequestCallbacks& callbacks) -> void {
            callbacks.complete(Filters::Common::RateLimit::LimitStatus::OK, nullptr, nullptr,
                               nullptr, "", nullptr);
          })));

  EXPECT_CALL(filter_callbacks_, continueDecoding()).Times(0);
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers_, false));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->decodeData(data_, false));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->decodeTrailers(request_trailers_));
  EXPECT_EQ(Http::Filter1xxHeadersStatus::Continue, filter_->encode1xxHeaders(response_headers_));
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers_, false));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(response_data_, false));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->encodeTrailers(response_trailers_));

  EXPECT_EQ(
      1U, filter_callbacks_.clusterInfo()->statsScope().counterFromStatName(ratelimit_ok_).value());
}

// Tests that the route rate limit is used when VhRateLimitsOptions::IGNORE and route rate limit is
// set
TEST_F(HttpRateLimitFilterTest, IgnoreVHRateLimitOptionWithRouteRateLimitSet) {
  setUpTest(filter_config_);
  envoy::extensions::filters::http::ratelimit::v3::RateLimitPerRoute settings;
  settings.set_vh_rate_limits(
      envoy::extensions::filters::http::ratelimit::v3::RateLimitPerRoute::IGNORE);
  absl::Status creation_status = absl::OkStatus();
  FilterConfigPerRoute per_route_config_(factory_context_, settings, creation_status);
  EXPECT_TRUE(creation_status.ok());

  EXPECT_CALL(filter_callbacks_.route_->route_entry_.rate_limit_policy_, getApplicableRateLimit(0));
  EXPECT_CALL(route_rate_limit_, populateDescriptors(_, _, _, _))
      .WillOnce(SetArgReferee<0>(descriptor_));

  EXPECT_CALL(filter_callbacks_.route_->route_entry_, includeVirtualHostRateLimits())
      .WillOnce(Return(false));

  EXPECT_CALL(*filter_callbacks_.route_, mostSpecificPerFilterConfig(_))
      .WillOnce(Return(&per_route_config_));

  EXPECT_CALL(filter_callbacks_.route_->virtual_host_->rate_limit_policy_,
              getApplicableRateLimit(0))
      .Times(0);

  EXPECT_CALL(*client_, limit(_, "foo",
                              testing::ContainerEq(std::vector<RateLimit::Descriptor>{
                                  {{{"descriptor_key", "descriptor_value"}}}}),
                              _, _, 0))
      .WillOnce(
          WithArgs<0>(Invoke([&](Filters::Common::RateLimit::RequestCallbacks& callbacks) -> void {
            callbacks.complete(Filters::Common::RateLimit::LimitStatus::OK, nullptr, nullptr,
                               nullptr, "", nullptr);
          })));

  EXPECT_CALL(filter_callbacks_, continueDecoding()).Times(0);
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers_, false));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->decodeData(data_, false));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->decodeTrailers(request_trailers_));
  EXPECT_EQ(Http::Filter1xxHeadersStatus::Continue, filter_->encode1xxHeaders(response_headers_));
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers_, false));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(response_data_, false));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->encodeTrailers(response_trailers_));

  EXPECT_EQ(
      1U, filter_callbacks_.clusterInfo()->statsScope().counterFromStatName(ratelimit_ok_).value());
}

// Tests that no rate limit is used when VhRateLimitsOptions::IGNORE is set and route rate limit
// empty
TEST_F(HttpRateLimitFilterTest, IgnoreVHRateLimitOptionWithOutRouteRateLimit) {
  setUpTest(filter_config_);
  envoy::extensions::filters::http::ratelimit::v3::RateLimitPerRoute settings;
  settings.set_vh_rate_limits(
      envoy::extensions::filters::http::ratelimit::v3::RateLimitPerRoute::IGNORE);
  absl::Status creation_status = absl::OkStatus();
  FilterConfigPerRoute per_route_config_(factory_context_, settings, creation_status);
  EXPECT_TRUE(creation_status.ok());

  EXPECT_CALL(filter_callbacks_.route_->route_entry_.rate_limit_policy_, getApplicableRateLimit(0));

  EXPECT_CALL(filter_callbacks_.route_->route_entry_, includeVirtualHostRateLimits())
      .WillOnce(Return(false));

  EXPECT_CALL(*filter_callbacks_.route_, mostSpecificPerFilterConfig(_))
      .WillOnce(Return(&per_route_config_));

  EXPECT_CALL(filter_callbacks_.route_->virtual_host_->rate_limit_policy_,
              getApplicableRateLimit(0))
      .Times(0);

  EXPECT_CALL(filter_callbacks_, continueDecoding()).Times(0);
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers_, false));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->decodeData(data_, false));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->decodeTrailers(request_trailers_));
  EXPECT_EQ(Http::Filter1xxHeadersStatus::Continue, filter_->encode1xxHeaders(response_headers_));
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers_, false));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(response_data_, false));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->encodeTrailers(response_trailers_));

  EXPECT_EQ(
      0, filter_callbacks_.clusterInfo()->statsScope().counterFromStatName(ratelimit_ok_).value());
}

// Tests that the domain is properly overridden when set at the per-route level
TEST_F(HttpRateLimitFilterTest, PerRouteDomainSet) {
  setUpTest(filter_config_);
  const std::string per_route_domain = "bar";
  envoy::extensions::filters::http::ratelimit::v3::RateLimitPerRoute settings;
  settings.set_domain(per_route_domain);
  absl::Status creation_status = absl::OkStatus();
  FilterConfigPerRoute per_route_config_(factory_context_, settings, creation_status);
  EXPECT_TRUE(creation_status.ok());

  EXPECT_CALL(filter_callbacks_.route_->route_entry_.rate_limit_policy_, getApplicableRateLimit(0));
  EXPECT_CALL(route_rate_limit_, populateDescriptors(_, _, _, _))
      .WillOnce(SetArgReferee<0>(descriptor_));

  EXPECT_CALL(filter_callbacks_.route_->route_entry_, includeVirtualHostRateLimits())
      .WillOnce(Return(false));

  EXPECT_CALL(*filter_callbacks_.route_, mostSpecificPerFilterConfig(_))
      .WillOnce(Return(&per_route_config_));

  EXPECT_CALL(*client_, limit(_, per_route_domain,
                              testing::ContainerEq(std::vector<RateLimit::Descriptor>{
                                  {{{"descriptor_key", "descriptor_value"}}}}),
                              _, _, 0))
      .WillOnce(
          WithArgs<0>(Invoke([&](Filters::Common::RateLimit::RequestCallbacks& callbacks) -> void {
            callbacks.complete(Filters::Common::RateLimit::LimitStatus::OK, nullptr, nullptr,
                               nullptr, "", nullptr);
          })));

  EXPECT_CALL(filter_callbacks_, continueDecoding()).Times(0);
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers_, false));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->decodeData(data_, false));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->decodeTrailers(request_trailers_));
  EXPECT_EQ(Http::Filter1xxHeadersStatus::Continue, filter_->encode1xxHeaders(response_headers_));
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers_, false));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(response_data_, false));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->encodeTrailers(response_trailers_));

  EXPECT_EQ(
      1U, filter_callbacks_.clusterInfo()->statsScope().counterFromStatName(ratelimit_ok_).value());
}

TEST_F(HttpRateLimitFilterTest, ConfigValueTest) {
  std::string stage_filter_config = R"EOF(
  {
    "domain": "foo",
    "stage": 5,
    "request_type" : "internal"
  }
  )EOF";

  setUpTest(stage_filter_config);

  EXPECT_EQ(5UL, config_->stage());
  EXPECT_EQ("foo", config_->domain());
  EXPECT_EQ(FilterRequestType::Internal, config_->requestType());
}

TEST_F(HttpRateLimitFilterTest, DefaultConfigValueTest) {
  std::string stage_filter_config = R"EOF(
  {
    "domain": "foo"
  }
  )EOF";

  setUpTest(stage_filter_config);

  EXPECT_EQ(0UL, config_->stage());
  EXPECT_EQ("foo", config_->domain());
  EXPECT_EQ(FilterRequestType::Both, config_->requestType());
}

// Test that defining stat_prefix appends an additional prefix to the emitted statistics names.
TEST_F(HttpRateLimitFilterTest, StatsWithPrefix) {
  const std::string stat_prefix = "with_stat_prefix";
  const std::string over_limit_counter_name_with_prefix =
      absl::StrCat("ratelimit.", stat_prefix, ".over_limit");
  const std::string over_limit_counter_name_without_prefix = "ratelimit.over_limit";

  setUpTest(stat_prefix_config_);
  InSequence s;

  EXPECT_CALL(route_rate_limit_, populateDescriptors(_, _, _, _))
      .WillOnce(SetArgReferee<0>(descriptor_));
  EXPECT_CALL(*client_, limit(_, _, _, _, _, 0))
      .WillOnce(
          WithArgs<0>(Invoke([&](Filters::Common::RateLimit::RequestCallbacks& callbacks) -> void {
            request_callbacks_ = &callbacks;
          })));

  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(request_headers_, false));

  EXPECT_CALL(filter_callbacks_.stream_info_,
              setResponseFlag(StreamInfo::CoreResponseFlag::RateLimited));

  Http::ResponseHeaderMapPtr h{new Http::TestResponseHeaderMapImpl()};
  Http::TestResponseHeaderMapImpl response_headers{
      {":status", "429"},
      {"x-envoy-ratelimited", Http::Headers::get().EnvoyRateLimitedValues.True}};
  EXPECT_CALL(filter_callbacks_, encodeHeaders_(HeaderMapEqualRef(&response_headers), true));
  EXPECT_CALL(filter_callbacks_, continueDecoding()).Times(0);

  request_callbacks_->complete(Filters::Common::RateLimit::LimitStatus::OverLimit, nullptr,
                               std::move(h), nullptr, "", nullptr);

  EXPECT_EQ(1U, filter_callbacks_.clusterInfo()
                    ->statsScope()
                    .counterFromString(over_limit_counter_name_with_prefix)
                    .value());

  EXPECT_EQ(0U, filter_callbacks_.clusterInfo()
                    ->statsScope()
                    .counterFromString(over_limit_counter_name_without_prefix)
                    .value());
  EXPECT_EQ(
      1U,
      filter_callbacks_.clusterInfo()->statsScope().counterFromStatName(upstream_rq_4xx_).value());
  EXPECT_EQ(
      1U,
      filter_callbacks_.clusterInfo()->statsScope().counterFromStatName(upstream_rq_429_).value());
  EXPECT_EQ("request_rate_limited", filter_callbacks_.details());
}

TEST(ObjectFactory, HitsAddend) {
  const std::string name = "envoy.ratelimit.hits_addend";
  auto* factory =
      Registry::FactoryRegistry<StreamInfo::FilterState::ObjectFactory>::getFactory(name);
  ASSERT_NE(nullptr, factory);
  EXPECT_EQ(name, factory->name());
  const std::string hits_addend = std::to_string(1234);
  auto object = factory->createFromBytes(hits_addend);
  ASSERT_NE(nullptr, object);
  EXPECT_EQ(hits_addend, object->serializeAsString());
}

TEST_F(HttpRateLimitFilterTest, PerRouteRateLimits) {
  const std::string route_config_yaml = R"EOF(
  domain: "bar"
  rate_limits:
  - actions:
    - request_headers:
        header_name: "x-header-name"
        descriptor_key: "header-name"
    hits_addend:
      format: "%REQ(x-test-hits-addend)%"
    )EOF";
  setUpTest(filter_config_, route_config_yaml);

  request_headers_.addCopy("x-header-name", "header-value");
  request_headers_.addCopy("x-test-hits-addend", "1234");

  EXPECT_CALL(*client_, limit(_, _, _, _, _, 0))
      .WillOnce(Invoke(
          [this](Filters::Common::RateLimit::RequestCallbacks& callbacks, const std::string& domain,
                 const std::vector<Envoy::RateLimit::Descriptor>& descriptors, Tracing::Span&,
                 OptRef<const StreamInfo::StreamInfo>, uint32_t) -> void {
            request_callbacks_ = &callbacks;
            EXPECT_EQ("bar", domain);
            EXPECT_EQ(1, descriptors.size());
            EXPECT_EQ("header-name", descriptors[0].entries_[0].key_);
            EXPECT_EQ("header-value", descriptors[0].entries_[0].value_);
            EXPECT_EQ(1234, descriptors[0].hits_addend_.value());
          }));

  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(request_headers_, false));

  EXPECT_CALL(filter_callbacks_.stream_info_,
              setResponseFlag(StreamInfo::CoreResponseFlag::RateLimited));

  Http::TestResponseHeaderMapImpl response_headers{
      {":status", "429"},
      {"x-envoy-ratelimited", Http::Headers::get().EnvoyRateLimitedValues.True}};
  EXPECT_CALL(filter_callbacks_, encodeHeaders_(HeaderMapEqualRef(&response_headers), true));
  EXPECT_CALL(filter_callbacks_, continueDecoding()).Times(0);

  request_callbacks_->complete(Filters::Common::RateLimit::LimitStatus::OverLimit, nullptr,
                               std::make_unique<Http::TestResponseHeaderMapImpl>(), nullptr, "",
                               nullptr);

  EXPECT_EQ(1U, filter_callbacks_.clusterInfo()
                    ->statsScope()
                    .counterFromStatName(ratelimit_over_limit_)
                    .value());

  EXPECT_EQ(
      1U,
      filter_callbacks_.clusterInfo()->statsScope().counterFromStatName(upstream_rq_4xx_).value());
  EXPECT_EQ(
      1U,
      filter_callbacks_.clusterInfo()->statsScope().counterFromStatName(upstream_rq_429_).value());
  EXPECT_EQ("request_rate_limited", filter_callbacks_.details());
}

TEST_F(HttpRateLimitFilterTest, PerRouteRateLimitsAndOnStreamDone) {
  const std::string route_config_yaml = R"EOF(
  domain: "bar"
  rate_limits:
  - actions:
    - request_headers:
        header_name: "x-header-name"
        descriptor_key: "header-name"
    hits_addend:
      format: "%REQ(x-test-hits-addend)%"
  - actions:
    - request_headers:
        header_name: "x-header-name"
        descriptor_key: "header-name"
    hits_addend:
      format: "%BYTES_RECEIVED%"
    apply_on_stream_done: true
    )EOF";
  setUpTest(filter_config_, route_config_yaml);

  request_headers_.addCopy("x-header-name", "header-value");
  request_headers_.addCopy("x-test-hits-addend", "1234");

  EXPECT_CALL(*client_, limit(_, _, _, _, _, 0))
      .WillOnce(Invoke(
          [this](Filters::Common::RateLimit::RequestCallbacks& callbacks, const std::string& domain,
                 const std::vector<Envoy::RateLimit::Descriptor>& descriptors, Tracing::Span&,
                 OptRef<const StreamInfo::StreamInfo>, uint32_t) -> void {
            request_callbacks_ = &callbacks;
            EXPECT_EQ("bar", domain);
            EXPECT_EQ(1, descriptors.size());
            EXPECT_EQ("header-name", descriptors[0].entries_[0].key_);
            EXPECT_EQ("header-value", descriptors[0].entries_[0].value_);
            EXPECT_EQ(1234, descriptors[0].hits_addend_.value());
          }));

  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(request_headers_, false));

  EXPECT_CALL(filter_callbacks_, continueDecoding());
  request_callbacks_->complete(Filters::Common::RateLimit::LimitStatus::OK, nullptr,
                               std::make_unique<Http::TestResponseHeaderMapImpl>(), nullptr, "",
                               nullptr);

  EXPECT_EQ(0U, filter_callbacks_.clusterInfo()
                    ->statsScope()
                    .counterFromStatName(ratelimit_over_limit_)
                    .value());

  filter_callbacks_.stream_info_.bytes_received_ = 789;

  EXPECT_CALL(*client_, limit(_, _, _, _, _, 0))
      .WillOnce(Invoke(
          [this](Filters::Common::RateLimit::RequestCallbacks& callbacks, const std::string& domain,
                 const std::vector<Envoy::RateLimit::Descriptor>& descriptors, Tracing::Span&,
                 OptRef<const StreamInfo::StreamInfo>, uint32_t) -> void {
            request_callbacks_ = &callbacks;
            EXPECT_EQ("bar", domain);
            EXPECT_EQ(1, descriptors.size());
            EXPECT_EQ("header-name", descriptors[0].entries_[0].key_);
            EXPECT_EQ("header-value", descriptors[0].entries_[0].value_);
            EXPECT_EQ(789, descriptors[0].hits_addend_.value());
          }));
  filter_->onDestroy();
  request_callbacks_->complete(Filters::Common::RateLimit::LimitStatus::OK, nullptr,
                               std::make_unique<Http::TestResponseHeaderMapImpl>(), nullptr, "",
                               nullptr);
}

TEST_F(HttpRateLimitFilterTest, FailureModeZeroPercentFailsOpen) {

  EXPECT_CALL(
      factory_context_.runtime_loader_.snapshot_,
      featureEnabled(absl::string_view("test.ratelimit.failure_mode_deny_percent"),
                     testing::Matcher<const envoy::type::v3::FractionalPercent&>(Percent(0))))
      .WillOnce(testing::Return(false));

  setUpTest(failure_mode_runtime_zero_percent_config_);
  InSequence s;

  EXPECT_CALL(route_rate_limit_, populateDescriptors(_, _, _, _))
      .WillOnce(SetArgReferee<0>(descriptor_));
  EXPECT_CALL(*client_, limit(_, _, _, _, _, 0))
      .WillOnce(
          WithArgs<0>(Invoke([&](Filters::Common::RateLimit::RequestCallbacks& callbacks) -> void {
            request_callbacks_ = &callbacks;
          })));

  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(request_headers_, false));

  EXPECT_CALL(filter_callbacks_, continueDecoding());

  request_callbacks_->complete(Filters::Common::RateLimit::LimitStatus::Error, nullptr, nullptr,
                               nullptr, "", nullptr);

  EXPECT_EQ(
      1U,
      filter_callbacks_.clusterInfo()->statsScope().counterFromStatName(ratelimit_error_).value());
  EXPECT_EQ(1U, filter_callbacks_.clusterInfo()
                    ->statsScope()
                    .counterFromStatName(ratelimit_failure_mode_allowed_)
                    .value());
}

TEST_F(HttpRateLimitFilterTest, FailureModeHundredPercentFailsClose) {

  EXPECT_CALL(
      factory_context_.runtime_loader_.snapshot_,
      featureEnabled(absl::string_view("test.ratelimit.failure_mode_deny_percent"),
                     testing::Matcher<const envoy::type::v3::FractionalPercent&>(Percent(100))))
      .WillOnce(testing::Return(true));

  setUpTest(failure_mode_runtime_hundred_percent_config_);
  InSequence s;

  EXPECT_CALL(route_rate_limit_, populateDescriptors(_, _, _, _))
      .WillOnce(SetArgReferee<0>(descriptor_));
  EXPECT_CALL(*client_, limit(_, _, _, _, _, 0))
      .WillOnce(
          WithArgs<0>(Invoke([&](Filters::Common::RateLimit::RequestCallbacks& callbacks) -> void {
            request_callbacks_ = &callbacks;
          })));

  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_->decodeHeaders(request_headers_, false));

  EXPECT_CALL(filter_callbacks_.stream_info_,
              setResponseFlag(StreamInfo::CoreResponseFlag::RateLimitServiceError));

  EXPECT_CALL(filter_callbacks_, encodeHeaders_(_, true))
      .WillOnce(Invoke([&](const Http::ResponseHeaderMap& headers, bool) -> void {
        EXPECT_EQ(headers.getStatusValue(),
                  std::to_string(enumToInt(Http::Code::InternalServerError)));
      }));

  request_callbacks_->complete(Filters::Common::RateLimit::LimitStatus::Error, nullptr, nullptr,
                               nullptr, "", nullptr);

  EXPECT_EQ(
      1U,
      filter_callbacks_.clusterInfo()->statsScope().counterFromStatName(ratelimit_error_).value());
  EXPECT_EQ(0U, filter_callbacks_.clusterInfo()
                    ->statsScope()
                    .counterFromStatName(ratelimit_failure_mode_allowed_)
                    .value());
}

} // namespace
} // namespace RateLimitFilter
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
