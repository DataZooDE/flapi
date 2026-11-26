// Exit-gate G1 (#211): refresh concurrency, database health, the cache
// registry and the cache routes.
#include <catch2/catch_test_macros.hpp>

// Pre-include STL headers before the private-to-public hack (see
// cache_manager_test.cpp).
#include <algorithm>
#include <any>
#include <atomic>
#include <chrono>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <crow.h>
#define private public
#include "../../src/include/cache_manager.hpp"
#include "../../src/include/api_server.hpp"
#undef private
#include "../../src/include/cache_refresh_command.hpp"
#include "../../src/include/config_service.hpp"
#include "../../src/include/database_health.hpp"
#include "../../src/include/database_manager.hpp"
#include "test_utils.hpp"

using namespace flapi;
using namespace flapi::test;

namespace {

// Records how many populate scripts run at once; each takes a while, so two
// unserialised refreshes overlap.
class ConcurrencyAdapter : public ICacheDatabaseAdapter {
public:
    std::atomic<int> running{0};
    std::atomic<int> max_running{0};
    std::atomic<int> populates{0};
    std::mutex tables_mutex;
    std::set<std::string> existing_tables;   // "schema.table"

    std::string renderCacheTemplate(const EndpointConfig&, const CacheConfig& cache,
                                    std::map<std::string, std::string>&) override {
        return "CREATE OR REPLACE TABLE x AS SELECT 1 -- " + cache.table;
    }
    void executeDuckLakeQuery(const std::string&, const std::map<std::string, std::string>&) override {}
    QueryResult executeDuckLakeQueryWithResult(const std::string& query) override {
        QueryResult result;
        std::vector<crow::json::wvalue> rows;
        if (query.find("duckdb_tables()") != std::string::npos) {
            std::lock_guard<std::mutex> lock(tables_mutex);
            for (const auto& name : existing_tables) {
                crow::json::wvalue row;
                row["schema_name"] = name.substr(0, name.find('.'));
                row["table_name"] = name.substr(name.find('.') + 1);
                rows.push_back(std::move(row));
            }
        }
        result.data = crow::json::wvalue(std::move(rows));
        return result;
    }
    std::optional<int64_t> executePopulateScript(const std::string&, const PopulateTarget&, bool) override {
        const int now = ++running;
        int seen = max_running.load();
        while (now > seen && !max_running.compare_exchange_weak(seen, now)) {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        --running;
        ++populates;
        return 1;
    }
};

std::string cacheConfig() {
    return R"(
project-name: g1-test
project-description: G1 regressions
http-port: 8080
template:
  path: ./sqls
ducklake:
  enabled: true
  alias: cache
  metadata-path: ./cache.ducklake
  data-path: ./cache_data
  warmup: never
connections:
  test:
    properties:
      path: ./data.parquet
)";
}

void writeCached(TempTestConfig& temp, const std::string& file, const std::string& path, const std::string& table,
                 const std::string& method = "GET") {
    temp.writeEndpoint(file, "url-path: " + path + "\nmethod: " + method +
                                 "\ntemplate-source: serve.sql\nconnection: [test]\ncache:\n  enabled: true\n"
                                 "  schema: main\n  table: " + table + "\n" +
                                 // A write endpoint's cache needs one (#276).
                                 (method == "GET" ? "" : "  template-file: serve.sql\n"));
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

TEST_CASE("Refreshes of different tables run one at a time; same-table duplicates coalesce",
          "[cache_manager][g1][regression]") {
    TempTestConfig temp(cacheConfig(), "g1_serialised");
    temp.writeSqlTemplate("serve.sql", "SELECT 1");
    writeCached(temp, "a.yaml", "/a", "a_cache");
    writeCached(temp, "b.yaml", "/b", "b_cache");
    writeCached(temp, "c.yaml", "/c", "c_cache");
    auto cm = temp.createConfigManager();
    auto adapter = std::make_shared<ConcurrencyAdapter>();
    CacheManager cache_manager(adapter);
    cache_manager.initializeReadiness(cm);

    SECTION("different tables: all succeed, never two populates at once") {
        std::vector<std::future<bool>> runs;
        for (const auto* table : {"a_cache", "b_cache", "c_cache"}) {
            runs.push_back(std::async(std::launch::async, [&, table]() {
                std::map<std::string, std::string> params;
                return cache_manager.refreshCache(cm, endpointFor(cm, table), params);
            }));
        }
        for (auto& run : runs) {
            REQUIRE(run.get());
        }
        REQUIRE(adapter->populates.load() == 3);
        REQUIRE(adapter->max_running.load() == 1);
    }

    SECTION("the same table twice: one runs, the other is coalesced (false)") {
        auto first = std::async(std::launch::async, [&]() {
            std::map<std::string, std::string> params;
            return cache_manager.refreshCache(cm, endpointFor(cm, "a_cache"), params);
        });
        while (adapter->running.load() == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::map<std::string, std::string> params;
        REQUIRE_FALSE(cache_manager.refreshCache(cm, endpointFor(cm, "a_cache"), params));
        REQUIRE(first.get());
        REQUIRE(adapter->populates.load() == 1);
    }
}

TEST_CASE("An invalidated database is fatal: /health and /health/live answer 503", "[health][g1][regression]") {
    DatabaseHealth::ResetForTesting();
    TempTestConfig temp("g1_fatal");
    auto config_manager = temp.createConfigManager();
    auto db_manager = std::make_shared<DatabaseManager>();
    APIServer server(config_manager, db_manager);

    REQUIRE(server.getLiveHealth().code == 200);

    // Any other error leaves it alone.
    DatabaseHealth::NoteDatabaseError("Binder Error: Referenced column x not found");
    REQUIRE_FALSE(DatabaseHealth::IsFatal());
    REQUIRE(server.getLiveHealth().code == 200);

    const std::string cause =
        "Query execution failed during cache refresh: FATAL Error: Failed: database has been invalidated because "
        "of a previous fatal error. The database must be restarted prior to being used again.\nOriginal error: "
        "\"Database name src was already used by a different database for this meta transaction\"";
    DatabaseHealth::NoteDatabaseError(cause);
    REQUIRE(DatabaseHealth::IsFatal());
    // The first cause is kept.
    DatabaseHealth::NoteDatabaseError("FATAL Error: database has been invalidated (a later one)");
    REQUIRE(DatabaseHealth::FatalReason()->find("meta transaction") != std::string::npos);

    for (int probe = 0; probe < 2; ++probe) {
        crow::response res = probe == 0 ? server.getLiveHealth() : server.getHealth();
        REQUIRE(res.code == 503);
        auto body = crow::json::load(res.body);
        REQUIRE(body);
        REQUIRE(std::string(body["status"].s()) == "fatal");
        REQUIRE(std::string(body["reason"].s()).find("invalidated") != std::string::npos);
    }
    auto refused = crow::json::load(DatabaseHealth::FatalResponseJson());
    REQUIRE(std::string(refused["error"].s()) == "database_invalidated");
    REQUIRE_FALSE(refused.has("reason"));   // the DuckDB text stays out of data responses

    DatabaseHealth::ResetForTesting();
    REQUIRE(server.getLiveHealth().code == 200);
}

TEST_CASE("The readiness registry follows the endpoint table", "[cache_manager][g1][regression]") {
    TempTestConfig temp(cacheConfig(), "g1_registry");
    temp.writeSqlTemplate("serve.sql", "SELECT 1");
    writeCached(temp, "a.yaml", "/a", "a_cache");
    auto cm = temp.createConfigManager();
    auto adapter = std::make_shared<ConcurrencyAdapter>();
    auto cache_manager = std::make_shared<CacheManager>(adapter);
    cache_manager->initializeReadiness(cm);
    cm->addEndpointsChangedListener([cache_manager, weak = std::weak_ptr<ConfigManager>(cm)]() {
        if (auto config = weak.lock()) {
            cache_manager->syncReadinessWithEndpoints(config);
        }
    });
    cache_manager->markCacheFailed(cm, endpointFor(cm, "a_cache"), "boom");
    REQUIRE(cache_manager->getReadinessSummary().failed == 1);

    SECTION("a changed table: the old entry goes, the new one is unbuilt") {
        EndpointConfig changed = endpointFor(cm, "a_cache");
        changed.cache.table = "b_cache";
        REQUIRE(cm->replaceEndpoint(changed));
        const auto summary = cache_manager->getReadinessSummary();
        REQUIRE(summary.total == 1);
        REQUIRE(summary.failed == 0);
        REQUIRE(summary.unbuilt == 1);
        REQUIRE(summary.unbuilt_caches.front().table == "b_cache");
        REQUIRE(cache_manager->readinessBlock(cm, endpointFor(cm, "b_cache")).has_value());
    }

    SECTION("a changed table that already exists is ready") {
        adapter->existing_tables = {"main.b_cache"};
        EndpointConfig changed = endpointFor(cm, "a_cache");
        changed.cache.table = "b_cache";
        REQUIRE(cm->replaceEndpoint(changed));
        const auto summary = cache_manager->getReadinessSummary();
        REQUIRE(summary.total == 1);
        REQUIRE(summary.ready == 1);
    }

    SECTION("an unchanged table keeps its state") {
        EndpointConfig same = endpointFor(cm, "a_cache");
        same.templateSource = "serve.sql";
        REQUIRE(cm->replaceEndpoint(same));
        REQUIRE(cache_manager->getReadinessSummary().failed == 1);
    }

    SECTION("a removed endpoint takes its entry and its stats with it") {
        std::map<std::string, std::string> params;
        adapter->existing_tables = {"main.a_cache"};
        REQUIRE(cache_manager->refreshCache(cm, endpointFor(cm, "a_cache"), params));
        REQUIRE(cache_manager->getRefreshStats().size() == 1);
        REQUIRE(cm->removeEndpoint(endpointFor(cm, "a_cache")));
        REQUIRE(cache_manager->getReadinessSummary().total == 0);
        REQUIRE(cache_manager->getRefreshStats().empty());
    }
}

TEST_CASE("Cache operations pick the cached endpoint of a shared path", "[config_service][g1][regression]") {
    TempTestConfig temp(cacheConfig(), "g1_cached_pick");
    temp.writeSqlTemplate("serve.sql", "SELECT 1");
    writeCached(temp, "orders_get.yaml", "/orders", "orders_cache");
    temp.writeEndpoint("orders_post.yaml",
                       "url-path: /orders\nmethod: POST\ntemplate-source: serve.sql\nconnection: [test]\n");
    auto cm = temp.createConfigManager();

    const auto lookup = FindCachedEndpointForPath(*cm, "/orders", "");
    REQUIRE(lookup.endpoint.has_value());
    REQUIRE(lookup.endpoint->method == "GET");
    const auto post = FindCachedEndpointForPath(*cm, "/orders", "post");
    REQUIRE_FALSE(post.endpoint.has_value());
    REQUIRE(post.found);
    REQUIRE(post.error.find("has no cache") != std::string::npos);
    const auto unknown = FindCachedEndpointForPath(*cm, "/nope", "");
    REQUIRE_FALSE(unknown.found);

    // The config service: only /cache/* routes resolve the cached one.
    const auto cached = ResolveSlugTarget(*cm, "-orders", nullptr, /*cached_only=*/true);
    REQUIRE(cached.ok);
    REQUIRE(cached.key == "GET /orders");
    const auto plain = ResolveSlugTarget(*cm, "-orders", nullptr);
    REQUIRE_FALSE(plain.ok);
    REQUIRE(plain.status == 400);
}

TEST_CASE("Two cached endpoints on one path and different tables stay ambiguous", "[config_service][g1]") {
    TempTestConfig temp(cacheConfig(), "g1_cached_ambiguous");
    temp.writeSqlTemplate("serve.sql", "SELECT 1");
    writeCached(temp, "orders_get.yaml", "/orders", "orders_cache");
    writeCached(temp, "orders_post.yaml", "/orders", "orders_other", "POST");
    auto cm = temp.createConfigManager();
    const auto lookup = FindCachedEndpointForPath(*cm, "/orders", "");
    REQUIRE_FALSE(lookup.endpoint.has_value());
    REQUIRE(lookup.error.find("--method") != std::string::npos);
    REQUIRE(FindCachedEndpointForPath(*cm, "/orders", "POST").endpoint->cache.table == "orders_other");
    REQUIRE_FALSE(ResolveSlugTarget(*cm, "-orders", nullptr, true).ok);
}

TEST_CASE("Replica wording and the audit row of catalog-wide jobs", "[cache_manager][g1][regression]") {
    CacheManager::CacheReadiness unbuilt;
    unbuilt.state = CacheManager::ReadinessState::Unbuilt;
    unbuilt.table = "t";
    REQUIRE(CacheManager::readinessBlockJson(unbuilt).dump().find("warm-up skipped") != std::string::npos);
    unbuilt.replica = true;
    const auto replica = CacheManager::readinessBlockJson(unbuilt).dump();
    REQUIRE(replica.find("writer has not built it yet") != std::string::npos);
    REQUIRE(replica.find("warm-up skipped") == std::string::npos);

    DuckLakeConfig ducklake;
    ducklake.read_only = true;
    ducklake.metadata_path = "postgres:dbname=lake";
    const auto attach = DatabaseManager::DuckLakeAttachErrorMessage(ducklake, "Catalog Error: no metadata");
    REQUIRE(attach.find("start the writer first") != std::string::npos);

    EndpointConfig catalog_wide;   // as catalogMaintenanceEndpoint() builds it
    catalog_wide.cache.schema = "";
    catalog_wide.cache.table = "";
    CacheManager::SyncEventDetails details;
    auto insert = CacheManager::buildSyncEventInsert("cache", "id", catalog_wide, "compaction", "success", "m",
                                                     details);
    REQUIRE(insert.find("'', '', 'compaction'") != std::string::npos);   // cache_table, cache_schema
    REQUIRE(insert.find(", false)") != std::string::npos);
    EndpointConfig table;
    table.cache.schema = "";
    table.cache.table = "t";
    details.full = true;
    insert = CacheManager::buildSyncEventInsert("cache", "id", table, "full", "success", "m", details);
    REQUIRE(insert.find("'t', 'main', 'full'") != std::string::npos);
    REQUIRE(insert.find(", true)") != std::string::npos);
}
