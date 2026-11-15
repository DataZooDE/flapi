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
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <memory>
#include <utility>
#include <vector>
#include <crow.h>
#define private public
#include "../../src/include/cache_manager.hpp"
#include "../../src/include/query_executor.hpp"
#undef private
#include "../../src/include/database_manager.hpp"
#include "../../src/include/heartbeat_worker.hpp"
#include "test_utils.hpp"

using namespace flapi;
using namespace flapi::test;

namespace {

// Records every statement; answers the snapshot listing with a configurable set and
// the live-table listing with one table, so retention can find expirable snapshots.
class SchedulerCacheAdapter : public ICacheDatabaseAdapter {
public:
    std::vector<std::string> queries;
    std::vector<std::map<std::string, std::string>> rendered_params;
    std::vector<std::int64_t> snapshot_ids;
    std::string live_schema = "main";
    std::string live_table = "orders";
    std::optional<std::int64_t> snapshot_ms;
    std::atomic<bool> block{false};
    std::atomic<bool> interrupted{false};
    std::string render_result = "CREATE OR REPLACE TABLE t AS SELECT 1";

    std::string renderCacheTemplate(const EndpointConfig&, const CacheConfig&,
                                    std::map<std::string, std::string>& params) override {
        rendered_params.push_back(params);
        return render_result;
    }

    void executeDuckLakeQuery(const std::string& query, const std::map<std::string, std::string>&) override {
        queries.push_back(query);
    }

    std::optional<int64_t> executePopulateScript(const std::string& script, const PopulateTarget&,
                                                 bool rebuild) override {
        queries.push_back(std::string(rebuild ? "REBUILD " : "") + script);
        // Stands in for a long warm-up query: spins until "interrupted".
        while (block.load() && !interrupted.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (interrupted.load()) {
            throw std::runtime_error("INTERRUPT Error: Interrupted!");
        }
        return 1;
    }

    QueryResult executeDuckLakeQueryWithResult(const std::string& query) override {
        queries.push_back(query);
        QueryResult result;
        std::vector<crow::json::wvalue> rows;
        if (query.find("duckdb_tables()") != std::string::npos) {
            crow::json::wvalue row;
            row["schema_name"] = live_schema;
            row["table_name"] = live_table;
            rows.push_back(std::move(row));
        } else if (query.find("ducklake_table_info") != std::string::npos) {
            crow::json::wvalue row;
            row["table_id"] = std::string("1");
            rows.push_back(std::move(row));
        } else if (query.find("epoch_ms(snapshot_time)") != std::string::npos) {
            if (snapshot_ms) {
                crow::json::wvalue row;
                row["ms"] = static_cast<double>(*snapshot_ms);
                rows.push_back(std::move(row));
            }
        } else if (query.find("ducklake_snapshots") != std::string::npos) {
            for (const auto id : snapshot_ids) {
                crow::json::wvalue row;
                row["snapshot_id"] = static_cast<double>(id);
                row["snapshot_time"] = "2026-01-01 00:00:00";
                row["exclusive"] = true;
                row["aged"] = true;
                rows.push_back(std::move(row));
            }
        }
        result.data = crow::json::wvalue(std::move(rows));
        return result;
    }

    std::size_t count(const std::string& needle) const {
        return static_cast<std::size_t>(std::count_if(queries.begin(), queries.end(), [&](const std::string& q) {
            return q.find(needle) != std::string::npos;
        }));
    }
};

std::string ducklakeConfig(const std::string& extra) {
    return R"(
project-name: scheduler-test
project-description: Scheduler test
http-port: 8080
template:
  path: ./sqls
ducklake:
  enabled: true
  alias: cache
  metadata-path: ./cache.ducklake
  data-path: ./cache_data
)" + extra + R"(
connections:
  test:
    properties:
      path: ./data.parquet
)";
}

void writeCached(TempTestConfig& temp, const std::string& name, const std::string& table,
                 const std::string& extra = "") {
    temp.writeEndpoint(name + ".yaml", "url-path: /" + name + "\n"
                       "method: GET\n"
                       "template-source: " + name + ".sql\n"
                       "connection: [test]\n"
                       "cache:\n"
                       "  enabled: true\n"
                       "  schema: main\n"
                       "  table: " + table + "\n" + extra);
    temp.writeSqlTemplate(name + ".sql", "SELECT 1");
}

EndpointConfig cachedEndpoint(const std::string& table) {
    EndpointConfig endpoint;
    endpoint.urlPath = "/" + table;
    endpoint.cache.enabled = true;
    endpoint.cache.schema = "main";
    endpoint.cache.table = table;
    return endpoint;
}

}  // namespace

TEST_CASE("A populate template's SELECT statements are found before anything runs", "[cache_manager][populate]") {
    REQUIRE(DatabaseManager::FindSelectStatement("SELECT 1") == std::optional<std::size_t>(1));
    REQUIRE(DatabaseManager::FindSelectStatement("FROM read_csv('x.csv')") == std::optional<std::size_t>(1));
    REQUIRE(DatabaseManager::FindSelectStatement(
                "CREATE OR REPLACE TABLE c.main.t AS SELECT 1;\nSELECT * FROM c.main.t") ==
            std::optional<std::size_t>(2));

    // Writes, however much SELECT they contain, are not bare SELECTs.
    REQUIRE_FALSE(DatabaseManager::FindSelectStatement("CREATE OR REPLACE TABLE c.main.t AS SELECT 1").has_value());
    REQUIRE_FALSE(DatabaseManager::FindSelectStatement(
                      "WITH s AS (SELECT 1 AS id) INSERT INTO c.main.t SELECT * FROM s").has_value());
    REQUIRE_FALSE(DatabaseManager::FindSelectStatement(
                      "-- SELECT 1\nINSERT INTO c.main.t VALUES ('SELECT')").has_value());
    REQUIRE_FALSE(DatabaseManager::FindSelectStatement(
                      "MERGE INTO c.main.t AS t USING (SELECT 1 AS id) AS s ON t.id = s.id "
                      "WHEN NOT MATCHED THEN INSERT *").has_value());
    // Unparseable: not judged here, execution reports it.
    REQUIRE_FALSE(DatabaseManager::FindSelectStatement("THIS IS NOT SQL").has_value());

    const auto message = DatabaseManager::PopulateSelectError(PopulateTarget{"cache", "main", "orders"}, 2);
    REQUIRE(message.find("cache template must write {{cache.catalog}}.{{cache.schema}}.{{cache.table}}") == 0);
    REQUIRE(message.find("CREATE OR REPLACE TABLE ... AS SELECT ...") != std::string::npos);
    REQUIRE(message.find("cache.main.orders") != std::string::npos);
    REQUIRE(message.find("statement 2") != std::string::npos);
}

TEST_CASE("Maintenance schedules accept intervals and @hourly/@daily/@weekly", "[config_manager][scheduler]") {
    REQUIRE(TimeInterval::parseSchedule("@hourly") == std::chrono::seconds(3600));
    REQUIRE(TimeInterval::parseSchedule("@daily") == std::chrono::seconds(86400));
    REQUIRE(TimeInterval::parseSchedule("@weekly") == std::chrono::seconds(7 * 86400));
    REQUIRE(TimeInterval::parseSchedule("6h") == std::chrono::seconds(6 * 3600));
    REQUIRE_FALSE(TimeInterval::parseSchedule("@yearly").has_value());
    REQUIRE_FALSE(TimeInterval::parseSchedule("0 3 * * *").has_value());
}

TEST_CASE("The cache scheduler is on unless the config opts out", "[config_manager][scheduler]") {
    SECTION("no scheduler block: on") {
        TempTestConfig temp(ducklakeConfig(""), "sched_default");
        auto cm = temp.createConfigManager();
        const auto& dl = cm->getDuckLakeConfig();
        REQUIRE(dl.scheduler.enabled);
        REQUIRE_FALSE(dl.scheduler.enabled_explicit);
        REQUIRE(HeartbeatWorker::ShouldRunCacheMaintenance(dl));
    }
    SECTION("a block without `enabled`: on") {
        TempTestConfig temp(ducklakeConfig("  scheduler:\n    scan-interval: 5s\n"), "sched_block");
        auto cm = temp.createConfigManager();
        const auto& dl = cm->getDuckLakeConfig();
        REQUIRE(dl.scheduler.enabled);
        REQUIRE(dl.scheduler.scan_interval.value() == "5s");
    }
    SECTION("explicit `enabled: false`: off") {
        TempTestConfig temp(ducklakeConfig("  scheduler:\n    enabled: false\n"), "sched_off");
        auto cm = temp.createConfigManager();
        const auto& dl = cm->getDuckLakeConfig();
        REQUIRE_FALSE(dl.scheduler.enabled);
        REQUIRE(dl.scheduler.enabled_explicit);
        REQUIRE_FALSE(HeartbeatWorker::ShouldRunCacheMaintenance(dl));
    }
    SECTION("an invalid scan-interval is refused at load") {
        TempTestConfig temp(ducklakeConfig("  scheduler:\n    scan-interval: soon\n"), "sched_bad_scan");
        REQUIRE_THROWS_AS(temp.createConfigManager(), ConfigurationError);
    }
    SECTION("an invalid compaction schedule is refused at load") {
        TempTestConfig temp(ducklakeConfig("  compaction:\n    enabled: true\n    schedule: '0 3 * * *'\n"),
                            "sched_bad_compaction");
        REQUIRE_THROWS_AS(temp.createConfigManager(), ConfigurationError);
    }
}

TEST_CASE("heartbeat.worker-interval has one default", "[config_manager][scheduler]") {
    SECTION("no heartbeat block") {
        TempTestConfig temp(ducklakeConfig(""), "hb_none");
        REQUIRE(temp.createConfigManager()->getGlobalHeartbeatConfig().workerInterval == std::chrono::seconds(60));
    }
    SECTION("a heartbeat block without worker-interval") {
        TempTestConfig temp(ducklakeConfig("") + "heartbeat:\n  enabled: false\n", "hb_block");
        REQUIRE(temp.createConfigManager()->getGlobalHeartbeatConfig().workerInterval == std::chrono::seconds(60));
    }
}

TEST_CASE("CacheScanInterval honours scan-interval, else the shortest schedule", "[heartbeat][scheduler]") {
    DuckLakeConfig ducklake;
    ducklake.enabled = true;
    GlobalHeartbeatConfig heartbeat;   // 60 s
    std::vector<EndpointConfig> endpoints;

    REQUIRE(HeartbeatWorker::CacheScanInterval(ducklake, heartbeat, endpoints) == std::chrono::seconds(60));

    auto fast = cachedEndpoint("fast");
    fast.cache.schedule = "10s";
    auto slow = cachedEndpoint("slow");
    slow.cache.schedule = "6h";
    endpoints = {slow, fast};
    REQUIRE(HeartbeatWorker::CacheScanInterval(ducklake, heartbeat, endpoints) == std::chrono::seconds(10));

    ducklake.scheduler.scan_interval = "2s";
    REQUIRE(HeartbeatWorker::CacheScanInterval(ducklake, heartbeat, endpoints) == std::chrono::seconds(2));

    // A disabled cache's schedule does not set the cadence.
    ducklake.scheduler.scan_interval.reset();
    endpoints[1].cache.enabled = false;
    REQUIRE(HeartbeatWorker::CacheScanInterval(ducklake, heartbeat, endpoints) == std::chrono::seconds(60));
}

TEST_CASE("RefreshDue counts from the last real refresh", "[heartbeat][scheduler]") {
    const auto now = std::chrono::system_clock::now();
    REQUIRE(HeartbeatWorker::RefreshDue(std::nullopt, std::chrono::seconds(60), now));
    REQUIRE_FALSE(HeartbeatWorker::RefreshDue(now - std::chrono::seconds(30), std::chrono::seconds(60), now));
    REQUIRE(HeartbeatWorker::RefreshDue(now - std::chrono::seconds(60), std::chrono::seconds(60), now));
}

TEST_CASE("Global ducklake.retention is the default for caches without their own", "[cache_manager][retention]") {
    TempTestConfig temp(ducklakeConfig("  retention:\n    keep-last-snapshots: 2\n"), "retention_global");
    writeCached(temp, "orders", "orders");
    auto cm = temp.createConfigManager();
    auto adapter = std::make_shared<SchedulerCacheAdapter>();
    adapter->snapshot_ids = {5, 4, 3, 2};
    CacheManager cache_manager(adapter);

    auto endpoint = cachedEndpoint("orders");
    SECTION("inherited") {
        const auto retention = CacheManager::effectiveRetention(cm, endpoint.cache);
        REQUIRE(retention.keep_last_snapshots == std::optional<std::size_t>(2));

        cache_manager.refreshDuckLakeCache(cm, endpoint, {});
        REQUIRE(adapter->count("ducklake_expire_snapshots('cache', versions => [3, 2])") == 1);
        // ...and the expired snapshots' files are deleted, not just unreferenced.
        REQUIRE(adapter->count("ducklake_cleanup_old_files('cache'") == 1);
    }
    SECTION("an endpoint's own block wins as a whole") {
        endpoint.cache.retention.max_snapshot_age = "7d";
        const auto retention = CacheManager::effectiveRetention(cm, endpoint.cache);
        REQUIRE_FALSE(retention.keep_last_snapshots.has_value());
        REQUIRE(retention.max_snapshot_age == std::optional<std::string>("7d"));
    }
}

TEST_CASE("Endpoints sharing a cache table are warmed once", "[cache_manager][warmup]") {
    TempTestConfig temp(ducklakeConfig("  warmup: always\n"), "warmup_dedupe");
    writeCached(temp, "a_read", "shared");
    writeCached(temp, "b_write", "shared");
    writeCached(temp, "c_other", "other");
    auto cm = temp.createConfigManager();

    const auto selected = CacheManager::warmupEndpoints(cm);
    REQUIRE(selected.size() == 2);

    auto adapter = std::make_shared<SchedulerCacheAdapter>();
    CacheManager cache_manager(adapter);
    cache_manager.warmUpCaches(cm);
    REQUIRE(adapter->count("CREATE OR REPLACE TABLE t AS SELECT 1") == 2);
}

TEST_CASE("A cache that failed to build answers cache_build_failed", "[cache_manager][readiness]") {
    CacheManager::CacheReadiness failed{CacheManager::ReadinessState::Failed, "cache", "main", "t", "boom"};
    auto body = CacheManager::readinessBlockJson(failed);
    auto parsed = crow::json::load(body.dump());
    REQUIRE(std::string(parsed["error"].s()) == "cache_build_failed");
    REQUIRE(std::string(parsed["message"].s()).find("failed to build") != std::string::npos);
    REQUIRE(CacheManager::readinessBlockResponse(failed).get_header_value("Retry-After") == "5");

    CacheManager::CacheReadiness starting{CacheManager::ReadinessState::Starting, "cache", "main", "t", ""};
    REQUIRE(std::string(crow::json::load(CacheManager::readinessBlockJson(starting).dump())["error"].s()) ==
            "cache_warming");
}

TEST_CASE("Audit event ids are random UUIDs", "[cache_manager][audit]") {
    static const std::regex uuid("^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$");
    std::set<std::string> seen;
    for (int i = 0; i < 1000; ++i) {
        const auto id = CacheManager::newEventId();
        REQUIRE(std::regex_match(id, uuid));
        seen.insert(id);
    }
    REQUIRE(seen.size() == 1000);
}

TEST_CASE("A full refresh renders without the watermark and rebuilds", "[cache_manager][full_refresh]") {
    TempTestConfig temp(ducklakeConfig(""), "full_refresh");
    auto cm = temp.createConfigManager();
    auto adapter = std::make_shared<SchedulerCacheAdapter>();
    adapter->snapshot_ids = {7};
    CacheManager cache_manager(adapter);
    auto endpoint = cachedEndpoint("orders");

    std::map<std::string, std::string> params;
    cache_manager.renderRefreshTemplate(cm, endpoint, params);
    REQUIRE(params.count("previousSnapshotTimestamp") == 1);
    REQUIRE(params.count("previousSnapshotId") == 1);

    cache_manager.renderRefreshTemplate(cm, endpoint, params, /*full=*/true);
    REQUIRE(params.count("previousSnapshotTimestamp") == 0);
    REQUIRE(params.count("previousSnapshotId") == 0);

    std::map<std::string, std::string> refresh_params;
    REQUIRE(cache_manager.refreshCache(cm, endpoint, refresh_params, CacheManager::RefreshOptions{true}));
    REQUIRE(adapter->count("REBUILD CREATE OR REPLACE TABLE t") == 1);
    REQUIRE(adapter->rendered_params.back().count("previousSnapshotTimestamp") == 0);
}

TEST_CASE("Shutdown interrupts a running refresh and refuses new ones", "[cache_manager][shutdown]") {
    TempTestConfig temp(ducklakeConfig(""), "shutdown_interrupt");
    auto cm = temp.createConfigManager();
    auto adapter = std::make_shared<SchedulerCacheAdapter>();
    adapter->block = true;
    CacheManager cache_manager(adapter);
    auto endpoint = cachedEndpoint("orders");

    auto running = std::async(std::launch::async, [&] {
        std::map<std::string, std::string> params;
        return cache_manager.refreshCache(cm, endpoint, params);
    });
    // Wait until the populate script is running.
    for (int i = 0; i < 400 && cache_manager.active_refresh_threads_.empty(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(cache_manager.active_refresh_threads_.size() == 1);

    // The mock cannot be reached by duckdb_interrupt; stand in for it.
    std::thread interrupter([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        adapter->interrupted = true;
    });
    const auto started = std::chrono::steady_clock::now();
    cache_manager.interruptRefreshesForShutdown(std::chrono::milliseconds(2000));
    interrupter.join();
    REQUIRE(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(2000));
    REQUIRE_THROWS(running.get());
    REQUIRE(cache_manager.shutdownRequested());

    adapter->block = false;
    adapter->interrupted = false;
    std::map<std::string, std::string> params;
    std::string refused;
    try {
        cache_manager.refreshCache(cm, endpoint, params);
    } catch (const std::exception& ex) {
        refused = ex.what();
    }
    REQUIRE(refused == "server is shutting down; refresh not started");
}

TEST_CASE("Compaction runs once per catalog and is audited", "[cache_manager][compaction]") {
    TempTestConfig temp(ducklakeConfig("  compaction:\n    enabled: true\n    schedule: '@daily'\n"), "compaction_once");
    writeCached(temp, "a", "a_cache");
    writeCached(temp, "b", "b_cache");
    auto cm = temp.createConfigManager();
    auto adapter = std::make_shared<SchedulerCacheAdapter>();
    CacheManager cache_manager(adapter);

    // Never compacted (no audit row): due.
    REQUIRE(cache_manager.compactionDue(cm, std::chrono::system_clock::now()));
    cache_manager.performCompaction(cm);
    REQUIRE(adapter->count("CALL ducklake_merge_adjacent_files('cache')") == 1);
    REQUIRE(adapter->count("CALL ducklake_rewrite_data_files('cache')") == 1);
    REQUIRE(adapter->count("ducklake_cleanup_old_files('cache'") == 1);
    REQUIRE(adapter->count("'compaction'") >= 1);
    // Not a refresh: it does not appear in the per-cache refresh stats.
    REQUIRE(cache_manager.getRefreshStats().empty());
}

TEST_CASE("The audit log is pruned as events accumulate", "[cache_manager][audit]") {
    TempTestConfig temp(ducklakeConfig(""), "audit_prune");
    auto cm = temp.createConfigManager();
    auto adapter = std::make_shared<SchedulerCacheAdapter>();
    adapter->live_schema = "audit";
    adapter->live_table = "sync_events";
    std::vector<std::int64_t> ids;
    for (std::int64_t i = 40; i > 0; --i) {
        ids.push_back(i);
    }
    adapter->snapshot_ids = ids;
    CacheManager cache_manager(adapter);
    auto endpoint = cachedEndpoint("orders");

    for (int i = 0; i < CacheManager::kAuditPruneEvery - 1; ++i) {
        cache_manager.recordSyncEvent(cm, endpoint, "full", "success");
    }
    REQUIRE(adapter->count("DELETE FROM \"cache\".audit.sync_events") == 0);
    cache_manager.recordSyncEvent(cm, endpoint, "full", "success");
    REQUIRE(adapter->count("DELETE FROM \"cache\".audit.sync_events") == 1);
    REQUIRE(adapter->count("rn > " + std::to_string(CacheManager::kAuditRowsKept)) == 1);
    // Only the newest audit snapshot (40) is kept; the rest of its history expired.
    REQUIRE(adapter->count("ducklake_expire_snapshots('cache', versions => [39, 38") == 1);
}

TEST_CASE("A failed warm-up over an existing cache table serves it stale", "[cache_manager][warmup][readiness]") {
    TempTestConfig temp(ducklakeConfig("  warmup: always\n"), "warmup_stale");
    writeCached(temp, "orders", "orders");
    writeCached(temp, "fresh", "fresh_cache");
    auto cm = temp.createConfigManager();
    auto adapter = std::make_shared<SchedulerCacheAdapter>();
    adapter->interrupted = true;   // every populate script fails: the source is down
    CacheManager cache_manager(adapter);
    cache_manager.warmUpCaches(cm);

    // main.orders exists (the adapter lists it): still served, marked stale.
    const auto orders = cm->getEndpointForPath("/orders");
    REQUIRE(orders);
    const auto existing = cache_manager.getEndpointReadiness(cm, *orders);
    REQUIRE(existing.state == CacheManager::ReadinessState::Ready);
    REQUIRE(existing.stale);
    REQUIRE_FALSE(cache_manager.readinessBlock(cm, *orders).has_value());

    // fresh_cache never existed: nothing to serve, so it is failed (503).
    const auto fresh = cm->getEndpointForPath("/fresh");
    REQUIRE(fresh);
    REQUIRE(cache_manager.getEndpointReadiness(cm, *fresh).state == CacheManager::ReadinessState::Failed);
}

TEST_CASE("The scheduler seeds from the newest snapshot of the table", "[cache_manager][scheduler]") {
    TempTestConfig temp(ducklakeConfig(""), "sched_seed");
    auto cm = temp.createConfigManager();
    auto adapter = std::make_shared<SchedulerCacheAdapter>();
    adapter->snapshot_ms = 1700000000000;
    CacheManager cache_manager(adapter);
    auto endpoint = cachedEndpoint("orders");

    const auto seeded = cache_manager.newestSnapshotTime(cm, endpoint);
    REQUIRE(seeded.has_value());
    REQUIRE(std::chrono::duration_cast<std::chrono::milliseconds>(seeded->time_since_epoch()).count() ==
            1700000000000);
    REQUIRE_FALSE(cache_manager.lastRefreshCompletedAt(cm, endpoint).has_value());

    std::map<std::string, std::string> params;
    cache_manager.refreshCache(cm, endpoint, params);
    REQUIRE(cache_manager.lastRefreshCompletedAt(cm, endpoint).has_value());
}

// Regression (#211): a cache block without template-file renders the endpoint's
// serving template, normally a bare SELECT. It used to build nothing; then the
// bare-SELECT refusal made the cache `failed` (examples/flapi-test.yaml's
// /customers/ answered 503). Materialised now, as a full refresh.
TEST_CASE("A cache without template-file materialises its serving SELECT", "[cache_manager][populate]") {
    TempTestConfig temp(ducklakeConfig(""), "materialise_select");
    auto cm = temp.createConfigManager();
    auto adapter = std::make_shared<SchedulerCacheAdapter>();
    adapter->render_result = "SELECT id, name FROM src WHERE 1=1\n";
    CacheManager cache_manager(adapter);
    auto endpoint = cachedEndpoint("orders");
    endpoint.cache.primary_keys = {"id"};
    endpoint.cache.cursor = CacheConfig::CursorConfig{"id", "integer"};   // merge mode by inference

    SECTION("no template-file: CREATE OR REPLACE TABLE ... AS <select>, recorded as full") {
        std::map<std::string, std::string> params;
        REQUIRE(cache_manager.refreshCache(cm, endpoint, params));
        REQUIRE(adapter->count("CREATE OR REPLACE TABLE \"cache\".\"main\".\"orders\" AS\nSELECT id, name FROM src") == 1);
        REQUIRE(adapter->count("INSERT INTO cache.audit.sync_events") == 1);
        REQUIRE(adapter->count("'full', 'success'") == 1);
    }
    SECTION("an explicit template-file is passed through, so a bare SELECT is still refused") {
        endpoint.cache.template_file = "orders_cache.sql";
        std::map<std::string, std::string> params;
        REQUIRE(cache_manager.refreshCache(cm, endpoint, params));
        REQUIRE(adapter->count("CREATE OR REPLACE TABLE \"cache\"") == 0);
        REQUIRE(adapter->count("'merge', 'success'") == 1);
    }
    SECTION("a multi-statement serving template is not rewritten") {
        adapter->render_result = "DELETE FROM x; SELECT 1";
        std::map<std::string, std::string> params;
        REQUIRE(cache_manager.refreshCache(cm, endpoint, params));
        REQUIRE(adapter->count("CREATE OR REPLACE TABLE \"cache\"") == 0);
    }
    REQUIRE(DatabaseManager::MaterialiseSelect("SELECT 1;", PopulateTarget{"c", "s", "t"}) ==
            std::optional<std::string>("CREATE OR REPLACE TABLE \"c\".\"s\".\"t\" AS\nSELECT 1;"));
    REQUIRE_FALSE(DatabaseManager::MaterialiseSelect("CREATE TABLE x AS SELECT 1", PopulateTarget{"c", "s", "t"}).has_value());
}
