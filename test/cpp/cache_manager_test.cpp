#include <catch2/catch_test_macros.hpp>
// Pre-include STL headers before the private-to-public hack
// to prevent "redeclared with different access" GCC errors
// when these headers are later included via crow/asio
#include <sstream>
#include <any>
#include <string>
#include <vector>
#include <map>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <optional>
#define private public
#include "../../src/include/cache_manager.hpp"
#include "../../src/include/query_executor.hpp"
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

    // Use explicit type to avoid ambiguous constructor call
    CacheManager cache_manager(std::shared_ptr<ICacheDatabaseAdapter>(nullptr));
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

// Simple recording adapter for testing CacheManager without hitting real DuckDB
class RecordingCacheAdapter : public ICacheDatabaseAdapter {
public:
    // Recorded calls
    std::vector<std::string> rendered_templates;
    std::vector<std::string> executed_queries;
    std::vector<std::map<std::string, std::string>> executed_params;

    // Control behavior
    std::string template_to_return = "SELECT 1";
    bool throw_on_execute = false;
    bool throw_on_snapshot_query = false;
    std::string exception_message = "Test exception";

    std::string renderCacheTemplate(const EndpointConfig& endpoint,
                                    const CacheConfig& cacheConfig,
                                    std::map<std::string, std::string>& params) override {
        // Record the params that were passed
        executed_params.push_back(params);
        rendered_templates.push_back(template_to_return);
        return template_to_return;
    }

    void executeDuckLakeQuery(const std::string& query,
                              const std::map<std::string, std::string>& params) override {
        executed_queries.push_back(query);
        if (throw_on_execute) {
            throw std::runtime_error(exception_message);
        }
    }

    QueryResult executeDuckLakeQueryWithResult(const std::string& query) override {
        executed_queries.push_back(query);
        if (throw_on_snapshot_query) {
            throw std::runtime_error(exception_message);
        }
        // Return empty result
        QueryResult result;
        result.data = crow::json::wvalue(crow::json::wvalue::list());
        return result;
    }
};

TEST_CASE("CacheManager refreshDuckLakeCache builds correct params", "[cache_manager]") {
    TempTestConfig temp("cache_refresh_params");
    auto config_manager = temp.createConfigManager();

    auto adapter = std::make_shared<RecordingCacheAdapter>();
    CacheManager cache_manager(adapter);

    EndpointConfig endpoint;
    endpoint.urlPath = "/test";
    endpoint.cache.enabled = true;
    endpoint.cache.table = "test_cache";
    endpoint.cache.schema = "analytics";

    SECTION("Basic params include catalog, schema, table") {
        std::map<std::string, std::string> params;
        cache_manager.refreshDuckLakeCache(config_manager, endpoint, params);

        REQUIRE(adapter->executed_params.size() >= 1);
        auto& captured_params = adapter->executed_params[0];

        REQUIRE(captured_params.count("cacheCatalog") == 1);
        REQUIRE(captured_params.count("cacheSchema") == 1);
        REQUIRE(captured_params.at("cacheSchema") == "analytics");
        REQUIRE(captured_params.count("cacheTable") == 1);
        REQUIRE(captured_params.at("cacheTable") == "test_cache");
        REQUIRE(captured_params.count("cacheMode") == 1);
        REQUIRE(captured_params.at("cacheMode") == "full");
    }

    SECTION("Schedule param included when configured") {
        endpoint.cache.schedule = "6h";
        std::map<std::string, std::string> params;
        cache_manager.refreshDuckLakeCache(config_manager, endpoint, params);

        auto& captured_params = adapter->executed_params[0];
        REQUIRE(captured_params.count("cacheSchedule") == 1);
        REQUIRE(captured_params.at("cacheSchedule") == "6h");
    }

    SECTION("Cursor params included when configured") {
        endpoint.cache.cursor = CacheConfig::CursorConfig{"updated_at", "timestamp"};
        std::map<std::string, std::string> params;
        cache_manager.refreshDuckLakeCache(config_manager, endpoint, params);

        auto& captured_params = adapter->executed_params[0];
        REQUIRE(captured_params.count("cursorColumn") == 1);
        REQUIRE(captured_params.at("cursorColumn") == "updated_at");
        REQUIRE(captured_params.count("cursorType") == 1);
        REQUIRE(captured_params.at("cursorType") == "timestamp");
        REQUIRE(captured_params.at("cacheMode") == "append");
    }

    SECTION("Primary keys param included when configured") {
        endpoint.cache.cursor = CacheConfig::CursorConfig{"updated_at", "timestamp"};
        endpoint.cache.primary_keys = {"id", "tenant_id"};
        std::map<std::string, std::string> params;
        cache_manager.refreshDuckLakeCache(config_manager, endpoint, params);

        auto& captured_params = adapter->executed_params[0];
        REQUIRE(captured_params.count("primaryKeys") == 1);
        REQUIRE(captured_params.at("primaryKeys") == "id,tenant_id");
        REQUIRE(captured_params.at("cacheMode") == "merge");
    }
}

TEST_CASE("CacheManager refreshDuckLakeCache snapshot fallback", "[cache_manager]") {
    TempTestConfig temp("cache_snapshot_fallback");
    auto config_manager = temp.createConfigManager();

    auto adapter = std::make_shared<RecordingCacheAdapter>();
    adapter->throw_on_snapshot_query = true;  // Simulate snapshot query failure

    CacheManager cache_manager(adapter);

    EndpointConfig endpoint;
    endpoint.urlPath = "/test";
    endpoint.cache.enabled = true;
    endpoint.cache.table = "test_cache";
    endpoint.cache.schema = "main";

    SECTION("Fallback snapshot info when query fails") {
        std::map<std::string, std::string> params;
        // Should not throw - fallback behavior kicks in
        REQUIRE_NOTHROW(cache_manager.refreshDuckLakeCache(config_manager, endpoint, params));

        // The refresh should still proceed with fallback snapshot info
        REQUIRE(adapter->rendered_templates.size() >= 1);
    }
}

TEST_CASE("CacheManager refreshDuckLakeCache retention SQL generation", "[cache_manager]") {
    TempTestConfig temp("cache_retention");
    auto config_manager = temp.createConfigManager();

    auto adapter = std::make_shared<RecordingCacheAdapter>();
    CacheManager cache_manager(adapter);

    EndpointConfig endpoint;
    endpoint.urlPath = "/test";
    endpoint.cache.enabled = true;
    endpoint.cache.table = "test_cache";
    endpoint.cache.schema = "main";

    SECTION("keep_last_snapshots generates versions-based expiry") {
        endpoint.cache.retention.keep_last_snapshots = 5;
        std::map<std::string, std::string> params;
        cache_manager.refreshDuckLakeCache(config_manager, endpoint, params);

        // Should have executed expire snapshots call
        bool found_expire = false;
        for (const auto& query : adapter->executed_queries) {
            if (query.find("ducklake_expire_snapshots") != std::string::npos &&
                query.find("versions") != std::string::npos) {
                found_expire = true;
                REQUIRE(query.find("5") != std::string::npos);
            }
        }
        REQUIRE(found_expire);
    }

    SECTION("max_snapshot_age generates time-based expiry") {
        endpoint.cache.retention.max_snapshot_age = "7 days";
        std::map<std::string, std::string> params;
        cache_manager.refreshDuckLakeCache(config_manager, endpoint, params);

        // Should have executed expire snapshots call with older_than
        bool found_expire = false;
        for (const auto& query : adapter->executed_queries) {
            if (query.find("ducklake_expire_snapshots") != std::string::npos &&
                query.find("older_than") != std::string::npos) {
                found_expire = true;
                REQUIRE(query.find("7 days") != std::string::npos);
            }
        }
        REQUIRE(found_expire);
    }

    SECTION("No retention config means no expire call") {
        // No retention configured
        std::map<std::string, std::string> params;
        cache_manager.refreshDuckLakeCache(config_manager, endpoint, params);

        // Should not have any expire snapshots calls
        for (const auto& query : adapter->executed_queries) {
            REQUIRE(query.find("ducklake_expire_snapshots") == std::string::npos);
        }
    }
}

TEST_CASE("CacheManager recordSyncEvent does not throw", "[cache_manager]") {
    TempTestConfig temp("cache_sync_event");
    auto config_manager = temp.createConfigManager();

    auto adapter = std::make_shared<RecordingCacheAdapter>();
    CacheManager cache_manager(adapter);

    EndpointConfig endpoint;
    endpoint.urlPath = "/test";
    endpoint.cache.enabled = true;
    endpoint.cache.table = "test_cache";
    endpoint.cache.schema = "main";

    SECTION("recordSyncEvent on success does not throw") {
        REQUIRE_NOTHROW(cache_manager.recordSyncEvent(
            config_manager, endpoint, "full", "success", "Cache refreshed"));
    }

    SECTION("recordSyncEvent on error does not throw") {
        REQUIRE_NOTHROW(cache_manager.recordSyncEvent(
            config_manager, endpoint, "full", "error", "Something went wrong"));
    }

    SECTION("recordSyncEvent with adapter failure does not throw") {
        adapter->throw_on_execute = true;
        REQUIRE_NOTHROW(cache_manager.recordSyncEvent(
            config_manager, endpoint, "full", "success", "Test message"));
    }
}
