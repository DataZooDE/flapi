#include <catch2/catch_test_macros.hpp>

// Pre-include STL headers before the private-to-public hack (see cache_manager_test.cpp).
#include <algorithm>
#include <any>
#include <atomic>
#include <chrono>
#include <future>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <crow.h>
#define private public
#include "../../src/include/cache_manager.hpp"
#include "../../src/include/query_executor.hpp"
#undef private
#include "test_utils.hpp"

using namespace flapi;
using namespace flapi::test;

namespace {

// Answers the live-table listing with a configurable set of tables, records every
// template render (a render means the populate query would have run).
class WarmupCacheAdapter : public ICacheDatabaseAdapter {
public:
    std::set<std::pair<std::string, std::string>> existing_tables;  // (schema, table)
    std::vector<std::string> rendered_tables;

    std::string renderCacheTemplate(const EndpointConfig&, const CacheConfig& cacheConfig,
                                    std::map<std::string, std::string>&) override {
        rendered_tables.push_back(cacheConfig.table);
        return "SELECT 1";
    }

    void executeDuckLakeQuery(const std::string&, const std::map<std::string, std::string>&) override {}

    QueryResult executeDuckLakeQueryWithResult(const std::string& query) override {
        QueryResult result;
        std::vector<crow::json::wvalue> rows;
        if (query.find("duckdb_tables()") != std::string::npos) {
            for (const auto& [schema, table] : existing_tables) {
                crow::json::wvalue row;
                row["schema_name"] = schema;
                row["table_name"] = table;
                rows.push_back(std::move(row));
            }
        }
        result.data = crow::json::wvalue(std::move(rows));
        return result;
    }
};

std::string configWithWarmup(const std::string& warmup_line) {
    return R"(
project-name: warmup-test
project-description: Warm-up mode test
http-port: 8080
template:
  path: ./sqls
ducklake:
  enabled: true
  alias: cache
  metadata-path: ./cache.ducklake
  data-path: ./cache_data
)" + warmup_line + R"(
connections:
  test:
    properties:
      path: ./data.parquet
)";
}

void writeCachedEndpoint(TempTestConfig& temp, const std::string& name, const std::string& extra = "") {
    temp.writeEndpoint(name + ".yaml", "url-path: /" + name + "\n"
                       "method: GET\n"
                       "template-source: " + name + ".sql\n"
                       "connection: [test]\n"
                       "cache:\n"
                       "  enabled: true\n"
                       "  schema: main\n"
                       "  table: " + name + "_cache\n" + extra);
    temp.writeSqlTemplate(name + ".sql", "SELECT 1");
}

const EndpointConfig& endpointFor(const std::shared_ptr<ConfigManager>& cm, const std::string& table) {
    for (const auto& endpoint : *cm->getEndpoints()) {
        if (endpoint.cache.table == table) {
            return endpoint;
        }
    }
    FAIL("no endpoint for " << table);
    throw std::logic_error("unreachable");
}

}  // namespace

TEST_CASE("Cache warm-up mode strings parse and print", "[cache_manager][warmup_mode]") {
    REQUIRE(ParseCacheWarmupMode("always") == CacheWarmupMode::Always);
    REQUIRE(ParseCacheWarmupMode("if-empty") == CacheWarmupMode::IfEmpty);
    REQUIRE(ParseCacheWarmupMode("never") == CacheWarmupMode::Never);
    REQUIRE_FALSE(ParseCacheWarmupMode("sometimes").has_value());
    REQUIRE_FALSE(ParseCacheWarmupMode("").has_value());
    REQUIRE(CacheWarmupModeToString(CacheWarmupMode::IfEmpty) == "if-empty");
}

TEST_CASE("ducklake.warmup defaults to always and is parsed", "[config_manager][warmup_mode]") {
    SECTION("default") {
        TempTestConfig temp(configWithWarmup(""), "warmup_default");
        auto cm = temp.createConfigManager();
        REQUIRE(cm->getDuckLakeConfig().warmup == CacheWarmupMode::Always);
    }
    SECTION("if-empty") {
        TempTestConfig temp(configWithWarmup("  warmup: if-empty\n"), "warmup_ifempty");
        auto cm = temp.createConfigManager();
        REQUIRE(cm->getDuckLakeConfig().warmup == CacheWarmupMode::IfEmpty);
    }
    SECTION("an unknown value is refused at load") {
        TempTestConfig temp(configWithWarmup("  warmup: sometimes\n"), "warmup_invalid");
        REQUIRE_THROWS(temp.createConfigManager());
    }
}

TEST_CASE("Effective warm-up mode: override > endpoint > global", "[config_manager][warmup_mode]") {
    TempTestConfig temp(configWithWarmup("  warmup: if-empty\n"), "warmup_precedence");
    writeCachedEndpoint(temp, "plain");
    writeCachedEndpoint(temp, "pinned", "  warmup: always\n");
    auto cm = temp.createConfigManager();

    const auto& plain = endpointFor(cm, "plain_cache");
    const auto& pinned = endpointFor(cm, "pinned_cache");
    REQUIRE(pinned.cache.warmup == CacheWarmupMode::Always);

    REQUIRE(cm->getEffectiveCacheWarmupMode(plain) == CacheWarmupMode::IfEmpty);
    REQUIRE(cm->getEffectiveCacheWarmupMode(pinned) == CacheWarmupMode::Always);

    cm->setCacheWarmupOverride(CacheWarmupMode::Never);
    REQUIRE(cm->getEffectiveCacheWarmupMode(plain) == CacheWarmupMode::Never);
    REQUIRE(cm->getEffectiveCacheWarmupMode(pinned) == CacheWarmupMode::Never);

    cm->setCacheWarmupOverride(std::nullopt);
    REQUIRE(cm->getEffectiveCacheWarmupMode(plain) == CacheWarmupMode::IfEmpty);
}

TEST_CASE("An invalid endpoint cache.warmup is refused at load", "[config_manager][warmup_mode]") {
    TempTestConfig temp(configWithWarmup(""), "warmup_endpoint_invalid");
    writeCachedEndpoint(temp, "bad", "  warmup: later\n");
    REQUIRE_THROWS(temp.createConfigManager());
}

TEST_CASE("warmUpCaches honours the warm-up mode", "[cache_manager][warmup_mode]") {
    auto adapter = std::make_shared<WarmupCacheAdapter>();
    adapter->existing_tables = {{"main", "built_cache"}};

    SECTION("always refreshes every cache, built or not") {
        TempTestConfig temp(configWithWarmup("  warmup: always\n"), "warmup_always");
        writeCachedEndpoint(temp, "built");
        writeCachedEndpoint(temp, "missing");
        auto cm = temp.createConfigManager();
        CacheManager cache_manager(adapter);
        adapter->rendered_tables.clear();
        cache_manager.warmUpCaches(cm);
        std::sort(adapter->rendered_tables.begin(), adapter->rendered_tables.end());
        REQUIRE(adapter->rendered_tables == std::vector<std::string>{"built_cache", "missing_cache"});
        REQUIRE(cache_manager.getReadinessSummary().ready == 2);
    }

    SECTION("if-empty refreshes only the cache whose table does not exist") {
        TempTestConfig temp(configWithWarmup("  warmup: if-empty\n"), "warmup_ifempty_run");
        writeCachedEndpoint(temp, "built");
        writeCachedEndpoint(temp, "missing");
        auto cm = temp.createConfigManager();
        CacheManager cache_manager(adapter);
        adapter->rendered_tables.clear();
        cache_manager.warmUpCaches(cm);
        REQUIRE(adapter->rendered_tables == std::vector<std::string>{"missing_cache"});
        REQUIRE(cache_manager.getEndpointReadiness(cm, endpointFor(cm, "built_cache")).state ==
                CacheManager::ReadinessState::Ready);
        REQUIRE(cache_manager.getEndpointReadiness(cm, endpointFor(cm, "missing_cache")).state ==
                CacheManager::ReadinessState::Ready);
    }

    SECTION("never runs no populate query; a missing table is Unbuilt, an existing one Ready") {
        TempTestConfig temp(configWithWarmup("  warmup: never\n"), "warmup_never_run");
        writeCachedEndpoint(temp, "built");
        writeCachedEndpoint(temp, "missing");
        auto cm = temp.createConfigManager();
        CacheManager cache_manager(adapter);
        adapter->rendered_tables.clear();
        cache_manager.warmUpCaches(cm);
        REQUIRE(adapter->rendered_tables.empty());

        const auto& built = endpointFor(cm, "built_cache");
        const auto& missing = endpointFor(cm, "missing_cache");
        REQUIRE(cache_manager.getEndpointReadiness(cm, built).state == CacheManager::ReadinessState::Ready);
        REQUIRE(cache_manager.getEndpointReadiness(cm, missing).state == CacheManager::ReadinessState::Unbuilt);

        auto summary = cache_manager.getReadinessSummary();
        REQUIRE(summary.total == 2);
        REQUIRE(summary.ready == 1);
        REQUIRE(summary.unbuilt == 1);
        REQUIRE(summary.failed == 0);
        REQUIRE(summary.pending_caches.empty());
        REQUIRE(summary.unbuilt_caches.size() == 1);
        REQUIRE(summary.unbuilt_caches.front().table == "missing_cache");

        // The request path refuses an unbuilt cache with its own error code.
        auto block = cache_manager.readinessBlock(cm, missing);
        REQUIRE(block.has_value());
        auto body = CacheManager::readinessBlockJson(*block);
        REQUIRE(body.dump().find("cache_not_built") != std::string::npos);
        REQUIRE_FALSE(cache_manager.readinessBlock(cm, built).has_value());
    }

    SECTION("the CLI/env override wins over the configured mode") {
        TempTestConfig temp(configWithWarmup("  warmup: always\n"), "warmup_override_run");
        writeCachedEndpoint(temp, "missing");
        auto cm = temp.createConfigManager();
        cm->setCacheWarmupOverride(CacheWarmupMode::Never);
        CacheManager cache_manager(adapter);
        adapter->rendered_tables.clear();
        cache_manager.warmUpCaches(cm);
        REQUIRE(adapter->rendered_tables.empty());
    }

    SECTION("a refresh that later succeeds turns Unbuilt into Ready") {
        TempTestConfig temp(configWithWarmup("  warmup: never\n"), "warmup_never_then_refresh");
        writeCachedEndpoint(temp, "missing");
        auto cm = temp.createConfigManager();
        CacheManager cache_manager(adapter);
        cache_manager.warmUpCaches(cm);
        const auto& missing = endpointFor(cm, "missing_cache");
        REQUIRE(cache_manager.getEndpointReadiness(cm, missing).state == CacheManager::ReadinessState::Unbuilt);
        std::map<std::string, std::string> params;
        REQUIRE(cache_manager.refreshCache(cm, missing, params));
        REQUIRE(cache_manager.getEndpointReadiness(cm, missing).state == CacheManager::ReadinessState::Ready);
    }
}
