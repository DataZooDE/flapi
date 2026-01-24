#include <catch2/catch_test_macros.hpp>
#define private public
#include "../../src/include/cache_manager.hpp"
#undef private
#include "test_utils.hpp"

using namespace flapi;
using namespace flapi::test;

TEST_CASE("CacheManager determineCacheMode respects cursor and primary keys", "[cache_manager]") {
    CacheConfig config;
    config.enabled = true;
    config.table = "customers";

    REQUIRE(CacheManager::determineCacheMode(config) == "full");

    config.cursor = CacheConfig::CursorConfig{"updated_at", "timestamp"};
    REQUIRE(CacheManager::determineCacheMode(config) == "append");

    config.primary_keys = {"id"};
    REQUIRE(CacheManager::determineCacheMode(config) == "merge");
}

TEST_CASE("CacheManager joinStrings produces comma separated values", "[cache_manager]") {
    std::vector<std::string> values = {"alpha", "beta", "gamma"};
    REQUIRE(CacheManager::joinStrings(values, ",") == "alpha,beta,gamma");
    values.clear();
    REQUIRE(CacheManager::joinStrings(values, ",").empty());
}

TEST_CASE("CacheManager addQueryCacheParamsIfNecessary toggles based on cache enablement", "[cache_manager]") {
    TempTestConfig temp("cache_manager_params");
    auto config_manager = temp.createConfigManager();

    CacheManager cache_manager(nullptr);
    EndpointConfig endpoint;
    endpoint.cache.enabled = false;
    endpoint.cache.table = "customers_cache";
    endpoint.cache.schema = "analytics";

    SECTION("Disabled cache does not add parameters") {
        std::map<std::string, std::string> params;
        cache_manager.addQueryCacheParamsIfNecessary(config_manager, endpoint, params);
        REQUIRE(params.empty());
    }

    SECTION("Enabled cache fills catalog, schema, and table") {
        endpoint.cache.enabled = true;
        std::map<std::string, std::string> params;
        cache_manager.addQueryCacheParamsIfNecessary(config_manager, endpoint, params);
        REQUIRE(params.at("cacheTable") == "customers_cache");
        REQUIRE(params.at("cacheSchema") == "analytics");
        REQUIRE(params.at("cacheCatalog") == config_manager->getDuckLakeConfig().alias);
    }
}

TEST_CASE("TimeInterval::parseInterval handles supported suffixes", "[cache_manager][time_interval]") {
    auto seconds = TimeInterval::parseInterval("15s");
    REQUIRE(seconds.has_value());
    REQUIRE(seconds.value() == std::chrono::seconds(15));

    auto minutes = TimeInterval::parseInterval("2m");
    REQUIRE(minutes.value() == std::chrono::seconds(120));

    auto hours = TimeInterval::parseInterval("3h");
    REQUIRE(hours.value() == std::chrono::seconds(3 * 3600));

    auto days = TimeInterval::parseInterval("1d");
    REQUIRE(days.value() == std::chrono::seconds(86400));

    auto invalid = TimeInterval::parseInterval("bad");
    REQUIRE_FALSE(invalid.has_value());
}
