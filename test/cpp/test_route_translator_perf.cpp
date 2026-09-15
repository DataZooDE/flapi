// Issue 0d - profiling the parked "server performance degrades under high
#include <algorithm>
// concurrent load" xfail, and guarding the fix.
//
// RouteTranslator::matchAndExtractParams rebuilds its std::regex on EVERY call,
// and translateRoutePath builds an additional std::regex per path parameter.
// ConfigManager::getEndpointForPathAndMethod then calls matchesPath in a linear
// scan over every endpoint, and three separate call sites do that per request
// (RateLimitMiddleware, AuthMiddleware, handleDynamicRequest).
//
// std::regex construction is famously expensive, so the per-request cost is
//     3 call sites x N endpoints x (1 + params) regex constructions.
//
// These tests pin the behaviour and the cost so the pre-compilation fix has a
// before/after number instead of an assertion of faith.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <map>
#include <string>
#include <vector>

#include "route_translator.hpp"

using namespace flapi;

namespace {

double medianMicros(const std::string& pattern, const std::string& path, int iters) {
    std::vector<double> samples;
    samples.reserve(iters);
    for (int i = 0; i < iters; ++i) {
        std::vector<std::string> names;
        std::map<std::string, std::string> params;
        auto t0 = std::chrono::steady_clock::now();
        volatile bool matched = RouteTranslator::matchAndExtractParams(pattern, path, names, params);
        auto t1 = std::chrono::steady_clock::now();
        (void)matched;
        samples.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
    }
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

}  // namespace

TEST_CASE("route matching is correct", "[route][perf]") {
    // Correctness first: the optimisation must not change any of this.
    SECTION("literal path matches itself") {
        std::vector<std::string> names;
        std::map<std::string, std::string> params;
        REQUIRE(RouteTranslator::matchAndExtractParams("/health", "/health", names, params));
        REQUIRE(params.empty());
    }
    SECTION("single parameter is extracted") {
        std::vector<std::string> names;
        std::map<std::string, std::string> params;
        REQUIRE(RouteTranslator::matchAndExtractParams(
            "/northwind/products/:product_id", "/northwind/products/42", names, params));
        REQUIRE(params.at("product_id") == "42");
    }
    SECTION("multiple parameters are extracted in order") {
        std::vector<std::string> names;
        std::map<std::string, std::string> params;
        REQUIRE(RouteTranslator::matchAndExtractParams(
            "/a/:x/b/:y", "/a/1/b/2", names, params));
        REQUIRE(params.at("x") == "1");
        REQUIRE(params.at("y") == "2");
    }
    SECTION("non-matching path is rejected") {
        std::vector<std::string> names;
        std::map<std::string, std::string> params;
        REQUIRE_FALSE(RouteTranslator::matchAndExtractParams("/a/:x", "/b/1", names, params));
    }
    SECTION("a parameter does not match across a segment boundary") {
        std::vector<std::string> names;
        std::map<std::string, std::string> params;
        REQUIRE_FALSE(RouteTranslator::matchAndExtractParams("/a/:x", "/a/1/2", names, params));
    }
}

TEST_CASE("route matching cost is bounded", "[route][perf]") {
    // The budget below is ~10x faster than the measured pre-fix cost and still
    // an order of magnitude above what a pre-compiled regex match needs, so it
    // fails loudly if the per-call regex construction ever comes back, without
    // being flaky on a loaded machine.
    constexpr double kBudgetMicros = 5.0;

    const double literal = medianMicros("/health", "/health", 2000);
    const double one_param =
        medianMicros("/northwind/products/:product_id", "/northwind/products/42", 2000);
    const double two_param = medianMicros("/a/:x/b/:y", "/a/1/b/2", 2000);

    WARN("median match cost: literal=" << literal << "us  1-param=" << one_param
         << "us  2-param=" << two_param << "us");

    CHECK(literal < kBudgetMicros);
    CHECK(one_param < kBudgetMicros);
    CHECK(two_param < kBudgetMicros);
}
