// Regression tests for the #211 cluster-C fixes that are testable without a
// running server: cache-not-ready 503s in the runtime metrics, percentile
// clamping, and the config-service route template used to label spans.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <string>

#include "config_service.hpp"
#include "runtime_metrics.hpp"

using flapi::ConfigService;
using flapi::RuntimeMetrics;
using std::chrono::microseconds;

TEST_CASE("A cache-not-ready 503 is not a server error", "[runtime_metrics][regression]") {
    auto& m = RuntimeMetrics::instance();
    m.reset();

    const std::string warming = R"({"error":"cache_warming","table":"t","message":"still being built"})";
    const std::string not_built = R"({"error":"cache_not_built","table":"t"})";
    const std::string busy = R"({"error":"The database is busy; retry shortly."})";

    REQUIRE(RuntimeMetrics::isCacheUnavailableResponse(503, warming));
    REQUIRE(RuntimeMetrics::isCacheUnavailableResponse(503, not_built));
    REQUIRE(RuntimeMetrics::isCacheUnavailableResponse(503, R"({"error":"cache_build_failed"})"));
    REQUIRE_FALSE(RuntimeMetrics::isCacheUnavailableResponse(503, busy));
    REQUIRE_FALSE(RuntimeMetrics::isCacheUnavailableResponse(500, warming));

    m.recordRest("GET", "/cached", 503, microseconds(100), RuntimeMetrics::isCacheUnavailableResponse(503, warming));
    m.recordRest("GET", "/cached", 503, microseconds(100), RuntimeMetrics::isCacheUnavailableResponse(503, busy));
    m.recordRest("GET", "/cached", 200, microseconds(100));

    const auto all = m.snapshot();
    REQUIRE(all.size() == 1);
    CHECK(all[0].requests == 3);
    CHECK(all[0].cache_unavailable == 1);
    CHECK(all[0].status_5xx == 1);  // only the real one

    const auto json = crow::json::load(m.toJson().dump());
    REQUIRE(json);
    CHECK(json["server_errors"].i() == 1);
    CHECK(json["cache_unavailable"].i() == 1);
    CHECK(json["series"][0]["status"]["503_cache"].i() == 1);
    CHECK(json["series"][0]["status"]["5xx"].i() == 1);
}

TEST_CASE("Percentiles never exceed the observed maximum", "[runtime_metrics][regression]") {
    auto& m = RuntimeMetrics::instance();
    m.reset();
    // 120 ms lands in the (100, 250] ms bucket: the unclamped p99 was 250.
    m.recordRest("GET", "/slow", 200, microseconds(120'000));
    const auto all = m.snapshot();
    REQUIRE(all.size() == 1);
    CHECK_THAT(all[0].max_ms, Catch::Matchers::WithinAbs(120.0, 1e-9));
    CHECK(all[0].p99_ms <= all[0].max_ms);
    CHECK_THAT(all[0].p50_ms, Catch::Matchers::WithinAbs(120.0, 1e-9));

    // Without a maximum the bucket bound is still what is reported.
    std::array<int64_t, RuntimeMetrics::kBucketCount> buckets{};
    buckets[RuntimeMetrics::bucketFor(120'000)] = 1;
    CHECK_THAT(RuntimeMetrics::percentileMs(buckets, 1, 0.99), Catch::Matchers::WithinAbs(250.0, 1e-9));
    CHECK_THAT(RuntimeMetrics::percentileMs(buckets, 1, 0.99, 120.0), Catch::Matchers::WithinAbs(120.0, 1e-9));
}

TEST_CASE("Config-service routes are labelled with their template", "[config_service][tracing][regression]") {
    CHECK(ConfigService::RouteTemplateFor("/api/v1/_config/endpoints") == "/api/v1/_config/endpoints");
    CHECK(ConfigService::RouteTemplateFor("/api/v1/_config/endpoints/customers-id") ==
          "/api/v1/_config/endpoints/<string>");
    CHECK(ConfigService::RouteTemplateFor("/api/v1/_config/endpoints/customers/reload?x=1") ==
          "/api/v1/_config/endpoints/<string>/reload");
    CHECK(ConfigService::RouteTemplateFor("/api/v1/_config/endpoints/orders/cache/refresh?full=true") ==
          "/api/v1/_config/endpoints/<string>/cache/refresh");
    CHECK(ConfigService::RouteTemplateFor("/api/v1/_config/endpoints/by-template") ==
          "/api/v1/_config/endpoints/by-template");
    CHECK(ConfigService::RouteTemplateFor("/api/v1/_config/metrics?endpoint=/secret") == "/api/v1/_config/metrics");
}
