#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "alloc_counter.hpp"
#include "runtime_metrics.hpp"

using flapi::RuntimeMetrics;
using flapi::test::AllocCounter;
using std::chrono::microseconds;

namespace {
const RuntimeMetrics::SeriesSnapshot* find(const std::vector<RuntimeMetrics::SeriesSnapshot>& all,
                                           const std::string& route) {
    for (const auto& s : all) {
        if (s.route == route) {
            return &s;
        }
    }
    return nullptr;
}
} // namespace

TEST_CASE("RuntimeMetrics counts REST requests by status class", "[runtime_metrics]") {
    auto& m = RuntimeMetrics::instance();
    m.reset();
    m.recordRest("GET", "/customers", 200, microseconds(800));
    m.recordRest("GET", "/customers", 404, microseconds(300));
    m.recordRest("GET", "/customers", 500, microseconds(40'000));
    m.recordRest("POST", "/customers", 201, microseconds(1'200));

    const auto all = m.snapshot();
    REQUIRE(all.size() == 2);  // GET and POST are separate series
    const auto* get = find(all, "/customers");
    REQUIRE(get != nullptr);
    const auto& g = get->method == "GET" ? *get : all[1];
    CHECK(g.requests == 3);
    CHECK(g.status_2xx == 1);
    CHECK(g.status_4xx == 1);
    CHECK(g.status_5xx == 1);
    CHECK(g.protocol == "rest");
    CHECK_THAT(g.max_ms, Catch::Matchers::WithinAbs(40.0, 1e-9));
}

TEST_CASE("RuntimeMetrics keeps MCP tools apart from REST routes", "[runtime_metrics]") {
    auto& m = RuntimeMetrics::instance();
    m.reset();
    m.recordMcpTool("customer_lookup", true, microseconds(500));
    m.recordMcpTool("customer_lookup", false, microseconds(700));
    const auto all = m.snapshot();
    REQUIRE(all.size() == 1);
    CHECK(all[0].protocol == "mcp");
    CHECK(all[0].requests == 2);
    CHECK(all[0].failures == 1);
}

TEST_CASE("RuntimeMetrics percentiles are bucket upper bounds", "[runtime_metrics]") {
    std::array<int64_t, RuntimeMetrics::kBucketCount> buckets{};
    // 90 requests <=1ms, 10 requests in the (25ms, 50ms] bucket
    buckets[RuntimeMetrics::bucketFor(900)] = 90;
    buckets[RuntimeMetrics::bucketFor(30'000)] = 10;
    CHECK_THAT(RuntimeMetrics::percentileMs(buckets, 100, 0.50), Catch::Matchers::WithinAbs(1.0, 1e-9));
    CHECK_THAT(RuntimeMetrics::percentileMs(buckets, 100, 0.90), Catch::Matchers::WithinAbs(1.0, 1e-9));
    CHECK_THAT(RuntimeMetrics::percentileMs(buckets, 100, 0.95), Catch::Matchers::WithinAbs(50.0, 1e-9));
    CHECK(RuntimeMetrics::percentileMs(buckets, 0, 0.5) == 0.0);
    // beyond the last finite bound lands in +Inf and reports the last bound
    CHECK(RuntimeMetrics::bucketFor(120'000'000) == RuntimeMetrics::kBucketCount - 1);
}

TEST_CASE("RuntimeMetrics counts rejections", "[runtime_metrics]") {
    auto& m = RuntimeMetrics::instance();
    m.reset();
    m.recordRejection(401);
    m.recordRejection(401);
    m.recordRejection(403);
    m.recordRejection(429);
    m.recordRejection(200);
    CHECK(m.unauthorized() == 2);
    CHECK(m.forbidden() == 1);
    CHECK(m.rateLimited() == 1);
}

TEST_CASE("RuntimeMetrics JSON filter narrows to one route", "[runtime_metrics]") {
    auto& m = RuntimeMetrics::instance();
    m.reset();
    m.recordRest("GET", "/a", 200, microseconds(100));
    m.recordRest("GET", "/b", 200, microseconds(100));
    const auto json = crow::json::load(m.toJson("/b").dump());
    REQUIRE(json);
    REQUIRE(json["series"].size() == 1);
    CHECK(json["series"][0]["route"].s() == "/b");
    CHECK(json["total"].i() == 1);
}

TEST_CASE("RuntimeMetrics is safe under concurrent recording", "[runtime_metrics]") {
    auto& m = RuntimeMetrics::instance();
    m.reset();
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&m, t] {
            const std::string route = "/r" + std::to_string(t % 3);
            for (int i = 0; i < 5'000; ++i) {
                m.recordRest("GET", route, 200, microseconds(i % 2'000));
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }
    int64_t total = 0;
    for (const auto& s : m.snapshot()) {
        total += s.requests;
    }
    CHECK(total == 8 * 5'000);
}

TEST_CASE("RuntimeMetrics recording a known series does not allocate", "[runtime_metrics][perf][alloc]") {
    // The hot-path budget: after the first request to an endpoint, recording
    // it again must cost no allocation (string_view lookup, atomics only).
    auto& m = RuntimeMetrics::instance();
    m.reset();
    m.recordRest("GET", "/customers/:id", 200, microseconds(100));
    m.recordMcpTool("customer_lookup", true, microseconds(100));
    std::size_t observed = 0;
    {
        AllocCounter c;
        for (int i = 0; i < 100; ++i) {
            m.recordRest("GET", "/customers/:id", 200, microseconds(100 + i));
            m.recordMcpTool("customer_lookup", true, microseconds(100 + i));
            m.recordRejection(429);
        }
        observed = c.count();
    }
    INFO("allocations observed: " << observed);
    REQUIRE(observed == 0);
}
