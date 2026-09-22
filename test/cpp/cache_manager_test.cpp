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
#include <atomic>
#include <future>
// crow must be parsed with its real access specifiers: including it under the
// hack below changes crow::response's layout in this TU only, so a default-
// constructed response disagrees with the one request_handler.cpp sees.
#include <crow.h>
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

    // Snapshot ids the next ducklake_snapshots() query should report, newest
    // first. Empty means "no snapshots", which is the common state early in a
    // cache's life.
    std::vector<std::int64_t> snapshot_ids;

    QueryResult executeDuckLakeQueryWithResult(const std::string& query) override {
        executed_queries.push_back(query);
        if (throw_on_snapshot_query) {
            throw std::runtime_error(exception_message);
        }
        QueryResult result;
        std::vector<crow::json::wvalue> rows;
        if (query.find("ducklake_snapshots") != std::string::npos) {
            for (const auto id : snapshot_ids) {
                crow::json::wvalue row;
                row["snapshot_id"] = static_cast<double>(id);
                row["snapshot_time"] = "2026-01-01 00:00:00";
                rows.push_back(std::move(row));
            }
        }
        result.data = crow::json::wvalue(std::move(rows));
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

    SECTION("keep_last_snapshots expires the snapshots beyond the newest N") {
        // This section previously asserted only that the SQL contained "5",
        // which the old implementation satisfied by emitting
        //   versions => ARRAY[0:5]
        // That is not DuckDB syntax - `SELECT ARRAY[0:10]` is a parser error -
        // so the CALL failed every time and the failure was swallowed at
        // WARNING level. The test passed while count-based retention had never
        // once run. It is rewritten here rather than kept, because what it
        // pinned was the defect.
        endpoint.cache.retention.keep_last_snapshots = 2;
        adapter->snapshot_ids = {50, 40, 30, 20, 10};   // newest first
        std::map<std::string, std::string> params;
        cache_manager.refreshDuckLakeCache(config_manager, endpoint, params);

        std::string expire;
        for (const auto& query : adapter->executed_queries) {
            if (query.find("ducklake_expire_snapshots") != std::string::npos) {
                expire = query;
            }
        }
        REQUIRE_FALSE(expire.empty());
        REQUIRE(expire.find("versions") != std::string::npos);
        // The two newest are KEPT; everything older is named explicitly.
        REQUIRE(expire.find("50") == std::string::npos);
        REQUIRE(expire.find("40") == std::string::npos);
        REQUIRE(expire.find("30") != std::string::npos);
        REQUIRE(expire.find("20") != std::string::npos);
        REQUIRE(expire.find("10") != std::string::npos);
        // And the list syntax is one DuckDB actually parses.
        REQUIRE(expire.find("ARRAY[0:") == std::string::npos);
    }

    SECTION("keep_last_snapshots expires nothing when there is nothing to expire") {
        // Fewer snapshots than the retention count is the normal early state.
        // Emitting a CALL with an empty version list would be an error.
        endpoint.cache.retention.keep_last_snapshots = 5;
        adapter->snapshot_ids = {20, 10};
        std::map<std::string, std::string> params;
        cache_manager.refreshDuckLakeCache(config_manager, endpoint, params);

        for (const auto& query : adapter->executed_queries) {
            REQUIRE(query.find("ducklake_expire_snapshots") == std::string::npos);
        }
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

TEST_CASE("CacheManager readiness transitions are tracked by table", "[cache_manager][readiness]") {
    TempTestConfig temp("cache_readiness");
    temp.writeEndpoint("cached.yaml", R"(
url-path: /cached
method: GET
template-source: cached.sql
connection: [test]
cache:
  enabled: true
  table: cached_table
)");
    temp.writeSqlTemplate("cached.sql", "SELECT 1");
    auto config_manager = temp.createConfigManager();
    const auto endpoint = config_manager->getEndpoints()->front();

    CacheManager cache_manager(std::shared_ptr<ICacheDatabaseAdapter>(nullptr));
    cache_manager.initializeReadiness(config_manager);

    auto starting = cache_manager.getEndpointReadiness(config_manager, endpoint);
    REQUIRE(starting.state == CacheManager::ReadinessState::Starting);

    cache_manager.markCacheReady(config_manager, endpoint);
    auto ready = cache_manager.getEndpointReadiness(config_manager, endpoint);
    REQUIRE(ready.state == CacheManager::ReadinessState::Ready);

    cache_manager.markCacheFailed(config_manager, endpoint, "boom");
    auto failed = cache_manager.getEndpointReadiness(config_manager, endpoint);
    REQUIRE(failed.state == CacheManager::ReadinessState::Failed);
    REQUIRE(failed.error == "boom");

    EndpointConfig uncached;
    auto unknown = cache_manager.getEndpointReadiness(config_manager, uncached);
    REQUIRE(unknown.state == CacheManager::ReadinessState::Ready);
}

class SelectiveThrowCacheAdapter : public RecordingCacheAdapter {
public:
    std::vector<std::string> refreshed_tables;

    std::string renderCacheTemplate(const EndpointConfig& endpoint,
                                    const CacheConfig& cacheConfig,
                                    std::map<std::string, std::string>& params) override {
        refreshed_tables.push_back(cacheConfig.table);
        if (cacheConfig.table == "first_cache") {
            throw std::runtime_error("first failed");
        }
        return RecordingCacheAdapter::renderCacheTemplate(endpoint, cacheConfig, params);
    }
};

TEST_CASE("CacheManager warmUpCaches records failures and continues", "[cache_manager][warmup]") {
    TempTestConfig temp("cache_warmup_failure");
    temp.writeEndpoint("first.yaml", R"(
url-path: /first
method: GET
template-source: first.sql
connection: [test]
cache:
  enabled: true
  table: first_cache
)");
    temp.writeSqlTemplate("first.sql", "SELECT 1");
    temp.writeEndpoint("second.yaml", R"(
url-path: /second
method: GET
template-source: second.sql
connection: [test]
cache:
  enabled: true
  table: second_cache
)");
    temp.writeSqlTemplate("second.sql", "SELECT 2");
    auto config_manager = temp.createConfigManager();
    auto adapter = std::make_shared<SelectiveThrowCacheAdapter>();
    CacheManager cache_manager(adapter);

    REQUIRE_NOTHROW(cache_manager.warmUpCaches(config_manager));

    bool saw_failed = false;
    bool saw_ready = false;
    for (const auto& endpoint : *config_manager->getEndpoints()) {
        auto readiness = cache_manager.getEndpointReadiness(config_manager, endpoint);
        if (endpoint.cache.table == "first_cache") {
            saw_failed = true;
            REQUIRE(readiness.state == CacheManager::ReadinessState::Failed);
            REQUIRE(readiness.error == "first failed");
        }
        if (endpoint.cache.table == "second_cache") {
            saw_ready = true;
            REQUIRE(readiness.state == CacheManager::ReadinessState::Ready);
        }
    }
    REQUIRE(saw_failed);
    REQUIRE(saw_ready);
    REQUIRE(adapter->refreshed_tables.size() == 2);
}

class SlowCountingCacheAdapter : public RecordingCacheAdapter {
public:
    std::atomic<int> refresh_count{0};
    std::mutex mutex;

    // RecordingCacheAdapter push_backs into shared vectors, so EVERY override
    // that reaches the base must hold the lock. renderCacheTemplate did not, and
    // the second half of this test refreshes two DIFFERENT tables concurrently -
    // so both threads rendered at once, raced on the same vectors, and corrupted
    // the heap. It surfaced as an intermittent SIGABRT (~7% of runs locally),
    // and the thread lambdas do not catch, so the abort took the process with it.
    std::string renderCacheTemplate(const EndpointConfig& endpoint,
                                    const CacheConfig& cacheConfig,
                                    std::map<std::string, std::string>& params) override {
        std::lock_guard<std::mutex> lock(mutex);
        return RecordingCacheAdapter::renderCacheTemplate(endpoint, cacheConfig, params);
    }

    void executeDuckLakeQuery(const std::string& query,
                              const std::map<std::string, std::string>& params) override {
        ++refresh_count;
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        std::lock_guard<std::mutex> lock(mutex);
        RecordingCacheAdapter::executeDuckLakeQuery(query, params);
    }

    QueryResult executeDuckLakeQueryWithResult(const std::string& query) override {
        std::lock_guard<std::mutex> lock(mutex);
        return RecordingCacheAdapter::executeDuckLakeQueryWithResult(query);
    }
};

TEST_CASE("CacheManager suppresses duplicate in-flight refreshes per table", "[cache_manager][inflight]") {
    TempTestConfig temp("cache_inflight");
    auto config_manager = temp.createConfigManager();
    auto adapter = std::make_shared<SlowCountingCacheAdapter>();
    CacheManager cache_manager(adapter);

    EndpointConfig one;
    one.urlPath = "/one";
    one.cache.enabled = true;
    one.cache.table = "same_cache";
    one.cache.schema = "main";
    std::map<std::string, std::string> params1;
    std::map<std::string, std::string> params2;

    // refreshCache rethrows, and an exception escaping a std::thread lambda is
    // std::terminate - an abort with no diagnostic instead of a readable failure.
    // Capture it so a future regression says what went wrong.
    std::mutex err_mutex;
    std::vector<std::string> errors;
    auto run = [&](const EndpointConfig& ep, std::map<std::string, std::string>& p) {
        try {
            cache_manager.refreshCache(config_manager, ep, p);
        } catch (const std::exception& ex) {
            std::lock_guard<std::mutex> lock(err_mutex);
            errors.emplace_back(ex.what());
        }
    };

    std::thread first([&]() { run(one, params1); });
    std::thread second([&]() { run(one, params2); });
    first.join();
    second.join();

    REQUIRE(errors.empty());
    REQUIRE(adapter->refresh_count.load() == 1);

    EndpointConfig two = one;
    two.cache.table = "other_cache";
    std::map<std::string, std::string> params3;
    std::map<std::string, std::string> params4;
    std::thread third([&]() { run(one, params3); });
    std::thread fourth([&]() { run(two, params4); });
    third.join();
    fourth.join();

    REQUIRE(errors.empty());
    REQUIRE(adapter->refresh_count.load() == 3);
}

class LatchingThrowCacheAdapter : public RecordingCacheAdapter {
public:
    std::promise<void> entered;
    std::shared_future<void> release;
    std::atomic<int> refresh_count{0};
    std::atomic<bool> signaled{false};

    explicit LatchingThrowCacheAdapter(std::shared_future<void> release_signal)
        : release(std::move(release_signal)) {
    }

    void executeDuckLakeQuery(const std::string& query,
                              const std::map<std::string, std::string>& params) override {
        (void)query;
        (void)params;
        ++refresh_count;
        bool expected = false;
        if (signaled.compare_exchange_strong(expected, true)) {
            entered.set_value();
            release.wait();
        }
        throw std::runtime_error("heartbeat refresh failed");
    }
};

TEST_CASE("CacheManager marks failed when duplicate warmup loses heartbeat race", "[cache_manager][warmup][inflight]") {
    TempTestConfig temp("cache_warmup_heartbeat_race");
    temp.writeEndpoint("cached.yaml", R"(
url-path: /cached
method: GET
template-source: cached.sql
connection: [test]
cache:
  enabled: true
  table: cached_table
)");
    temp.writeSqlTemplate("cached.sql", "SELECT 1");
    auto config_manager = temp.createConfigManager();
    const auto endpoint = config_manager->getEndpoints()->front();

    std::promise<void> release_refresh;
    auto release_future = release_refresh.get_future().share();
    auto adapter = std::make_shared<LatchingThrowCacheAdapter>(release_future);
    CacheManager cache_manager(adapter);
    cache_manager.initializeReadiness(config_manager);
    cache_manager.markCacheStarting(config_manager, endpoint);

    std::map<std::string, std::string> params;
    std::thread heartbeat([&]() {
        try {
            cache_manager.refreshCache(config_manager, endpoint, params);
        } catch (const std::exception&) {
        }
    });

    adapter->entered.get_future().wait();

    std::thread warmup([&]() {
        REQUIRE_NOTHROW(cache_manager.warmUpCaches(config_manager));
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(25));

    release_refresh.set_value();
    heartbeat.join();
    warmup.join();

    auto readiness = cache_manager.getEndpointReadiness(config_manager, endpoint);
    REQUIRE(readiness.state == CacheManager::ReadinessState::Failed);
    REQUIRE(readiness.error == "heartbeat refresh failed");
    REQUIRE(adapter->refresh_count.load() >= 1);
}
