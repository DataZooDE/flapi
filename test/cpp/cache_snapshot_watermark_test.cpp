#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "cache_database_adapter.hpp"
#include "cache_manager.hpp"
#include "config_manager.hpp"
#include "database_manager.hpp"

using namespace flapi;
namespace fs = std::filesystem;

// The incremental cache watermark must be per TABLE, not per catalog.
//
// Every flAPI cache shares one DuckLake catalog. fetchSnapshotInfo took the
// newest two snapshots in that catalog, so as soon as a second endpoint was
// cached, another table's refresh became this table's "previous snapshot".
// Its timestamp is NEWER than this table's real previous refresh, and
// {{cache.previousSnapshotTimestamp}} drives the incremental WHERE clause -
// so every row changed in between was skipped, and the refresh reported
// success.
//
// The FIRST version of this test did not test any of that. It reimplemented
// the new SQL inside the test and asserted against its own copy, never once
// calling CacheManager - `grep -c CacheManager` on it returned 0 - so it
// passed unchanged against the catalog-wide code it was supposed to pin. A
// crew review caught it. It is rewritten here to drive the real code.
namespace {

// Real query execution against a real DuckLake catalog, so the per-table
// filtering in fetchSnapshotInfo actually runs - plus capture of the params
// CacheManager hands to the renderer, which is where the chosen watermark
// becomes observable from outside.
class CapturingRealAdapter : public ICacheDatabaseAdapter {
public:
    explicit CapturingRealAdapter(std::shared_ptr<DatabaseManager> db) : db_(std::move(db)) {}

    std::string renderCacheTemplate(const EndpointConfig&,
                                    const CacheConfig&,
                                    std::map<std::string, std::string>& params) override {
        captured_params = params;
        ++render_calls;
        return "SELECT 1";
    }

    void executeDuckLakeQuery(const std::string& query,
                              const std::map<std::string, std::string>& = {}) override {
        executed.push_back(query);
        std::map<std::string, std::string> p;
        db_->executeQuery(query, p, false);
    }

    QueryResult executeDuckLakeQueryWithResult(const std::string& query) override {
        executed.push_back(query);
        std::map<std::string, std::string> p;
        return db_->executeQuery(query, p, false);
    }

    std::map<std::string, std::string> captured_params;
    std::vector<std::string> executed;
    int render_calls = 0;

private:
    std::shared_ptr<DatabaseManager> db_;
};

EndpointConfig cachedEndpoint(const std::string& url, const std::string& table) {
    EndpointConfig e;
    e.urlPath = url;
    e.method = "GET";
    e.templateSource = "x.sql";
    e.cache.enabled = true;
    e.cache.table = table;
    e.cache.schema = "s";
    return e;
}

}  // namespace

TEST_CASE("the incremental watermark comes from the table's own snapshots",
          "[cache][ducklake][watermark]") {
    fs::path temp_dir = fs::temp_directory_path() / "flapi_watermark_real";
    fs::remove_all(temp_dir);
    fs::create_directories(temp_dir / "data");
    fs::path config_path = temp_dir / "config.yaml";

    {
        std::ofstream cfg(config_path);
        cfg << R"(
project-name: watermark_test
project-description: per-table snapshot watermark

template:
  path: )" << temp_dir.string() << R"(

duckdb:
  db_path: )" << (temp_dir / "wm.db").string() << R"(

ducklake:
  enabled: true
  alias: cache
  metadata-path: )" << (temp_dir / "metadata.ducklake").string() << R"(
  data-path: )" << (temp_dir / "data").string() << R"(

connections:
  default:
    init: "SELECT 1;"
)";
    }

    auto config_manager = std::make_shared<ConfigManager>(config_path);
    config_manager->loadConfig();
    auto db = DatabaseManager::getInstance();
    db->reset();
    REQUIRE_NOTHROW(db->initializeDBManagerFromConfig(config_manager));

    auto adapter = std::make_shared<CapturingRealAdapter>(db);
    CacheManager cache_manager(adapter);

    std::map<std::string, std::string> p;
    db->executeQuery("CREATE SCHEMA IF NOT EXISTS cache.s", p, false);
    // A moves twice, then B moves. So the newest snapshot in the CATALOG
    // belongs to B, while A's own latest - the watermark A's next incremental
    // refresh must use - is older. A catalog-wide lookup returns B's, which is
    // NEWER than anything A wrote, so A would skip its own rows.
    db->executeQuery("CREATE TABLE cache.s.a AS SELECT 1 AS i", p, false);
    db->executeQuery("INSERT INTO cache.s.a VALUES (2)", p, false);
    db->executeQuery("CREATE TABLE cache.s.b AS SELECT 1 AS i", p, false);

    auto snapshotIds = [&](const std::string& sql) {
        std::map<std::string, std::string> q;
        auto r = db->executeQuery(sql, q, false);
        auto rows = crow::json::load(r.data.dump());
        std::vector<int64_t> out;
        if (rows && rows.t() == crow::json::type::List) {
            for (size_t i = 0; i < rows.size(); ++i) {
                out.push_back(static_cast<int64_t>(rows[i]["snapshot_id"].d()));
            }
        }
        return out;
    };
    const auto catalog_wide = snapshotIds(
        "SELECT snapshot_id FROM ducklake_snapshots('cache') ORDER BY snapshot_id DESC LIMIT 2");
    REQUIRE(catalog_wide.size() == 2);

    // Drive the REAL CacheManager. This is what the previous version of the
    // test never did.
    std::map<std::string, std::string> params;
    cache_manager.refreshDuckLakeCache(config_manager, cachedEndpoint("/a", "a"), params);
    REQUIRE(adapter->render_calls == 1);

    SECTION("the watermark is this table's own latest snapshot") {
        const auto it = adapter->captured_params.find("previousSnapshotId");
        REQUIRE(it != adapter->captured_params.end());
        const int64_t chosen = std::stoll(it->second);

        // Catalog-wide, the newest snapshot is b's. Using it as a's watermark
        // would skip every row a wrote before b moved.
        REQUIRE(chosen != catalog_wide[0]);
        REQUIRE(chosen < catalog_wide[0]);

        // And it is the LAST COMPLETED refresh of a, not the one before that.
        // These used to carry index 1 - two refreshes back - so an append
        // template re-read rows the previous refresh had already appended.
        std::map<std::string, std::string> q;
        auto own = db->executeQuery(
            "SELECT snapshot_id FROM ducklake_snapshots('cache') "
            "WHERE list_contains(flatten(map_values(changes)), "
            "  (SELECT CAST(table_id AS VARCHAR) FROM ducklake_table_info('cache') "
            "   WHERE table_name = 'a' LIMIT 1)) "
            "   OR list_contains(flatten(map_values(changes)), 's.a') "
            "ORDER BY snapshot_id DESC LIMIT 2", q, false);
        auto rows = crow::json::load(own.data.dump());
        REQUIRE(rows.size() == 2);
        const int64_t a_latest = static_cast<int64_t>(rows[0]["snapshot_id"].d());
        const int64_t a_previous = static_cast<int64_t>(rows[1]["snapshot_id"].d());
        REQUIRE(chosen == a_latest);
        REQUIRE(chosen != a_previous);
    }

    SECTION("the query CacheManager issued is scoped to the table") {
        // Direct evidence that the product builds a per-table query, rather
        // than the test asserting against SQL it wrote itself.
        //
        // The assertion is on the SCOPING, not on how the table is named. The
        // table id used to be inlined as a correlated subquery; it is now
        // resolved by its own ducklake_table_info query, because DuckDB
        // refuses a subquery inside a lambda body and the retention filter
        // needs one. Pinning the old shape would fail that refactor while the
        // contract held.
        bool queried_snapshots = false;
        bool resolved_table = false;
        for (const auto& q : adapter->executed) {
            if (q.find("ducklake_table_info") != std::string::npos &&
                q.find("table_name = 'a'") != std::string::npos) {
                resolved_table = true;
            }
            if (q.find("ducklake_snapshots") != std::string::npos) {
                queried_snapshots = true;
                // Scoped by what the snapshot touched, and naming THIS table.
                REQUIRE(q.find("changes") != std::string::npos);
                REQUIRE(q.find("'s.a'") != std::string::npos);
                // ...and never the other table's.
                REQUIRE(q.find("'s.b'") == std::string::npos);
            }
        }
        REQUIRE(queried_snapshots);
        REQUIRE(resolved_table);
    }

    db->reset();
    fs::remove_all(temp_dir);
}

TEST_CASE("count-based retention actually expires snapshots",
          "[cache][ducklake][retention]") {
    // keep-last-snapshots emitted
    //     CALL ducklake_expire_snapshots(..., versions => ARRAY[0:N])
    // and `SELECT ARRAY[0:10]` is a DuckDB parser error, so the CALL failed
    // every time. The failure was caught and logged at WARNING, so count-based
    // retention had never once run while appearing configured.
    //
    // The integration test that was meant to cover this iterated over GC audit
    // events and accepted `status in ["success", "error"]` - so it passed with
    // zero events, and passed on failure when there were any. This drives the
    // real CacheManager against a real catalog and requires the expiry to
    // happen.
    fs::path temp_dir = fs::temp_directory_path() / "flapi_retention_real";
    fs::remove_all(temp_dir);
    fs::create_directories(temp_dir / "data");
    fs::path config_path = temp_dir / "config.yaml";

    {
        std::ofstream cfg(config_path);
        cfg << R"(
project-name: retention_test
project-description: count-based retention must run

template:
  path: )" << temp_dir.string() << R"(

duckdb:
  db_path: )" << (temp_dir / "rt.db").string() << R"(

ducklake:
  enabled: true
  alias: cache
  metadata-path: )" << (temp_dir / "metadata.ducklake").string() << R"(
  data-path: )" << (temp_dir / "data").string() << R"(

connections:
  default:
    init: "SELECT 1;"
)";
    }

    auto config_manager = std::make_shared<ConfigManager>(config_path);
    config_manager->loadConfig();
    auto db = DatabaseManager::getInstance();
    db->reset();
    REQUIRE_NOTHROW(db->initializeDBManagerFromConfig(config_manager));

    auto adapter = std::make_shared<CapturingRealAdapter>(db);
    CacheManager cache_manager(adapter);

    std::map<std::string, std::string> p;
    db->executeQuery("CREATE SCHEMA IF NOT EXISTS cache.s", p, false);
    db->executeQuery("CREATE TABLE cache.s.r AS SELECT 1 AS i", p, false);
    for (int i = 2; i <= 6; ++i) {
        db->executeQuery("INSERT INTO cache.s.r VALUES (" + std::to_string(i) + ")", p, false);
    }

    auto snapshotCount = [&]() {
        std::map<std::string, std::string> q;
        auto r = db->executeQuery(
            "SELECT count(*) AS n FROM ducklake_snapshots('cache')", q, false);
        auto rows = crow::json::load(r.data.dump());
        return static_cast<int64_t>(rows[0]["n"].d());
    };
    const int64_t before = snapshotCount();
    REQUIRE(before > 2);

    auto endpoint = cachedEndpoint("/r", "r");
    endpoint.cache.retention.keep_last_snapshots = 2;

    std::map<std::string, std::string> params;
    cache_manager.refreshDuckLakeCache(config_manager, endpoint, params);

    SECTION("an expire call was issued, with real snapshot ids") {
        std::string expire;
        for (const auto& q : adapter->executed) {
            if (q.find("ducklake_expire_snapshots") != std::string::npos) {
                expire = q;
            }
        }
        REQUIRE_FALSE(expire.empty());
        REQUIRE(expire.find("versions") != std::string::npos);
        // The shape DuckDB rejects must not come back.
        REQUIRE(expire.find("ARRAY[0:") == std::string::npos);
    }

    SECTION("and the snapshots are actually gone") {
        // The assertion the old test could not make: the expiry took effect.
        REQUIRE(snapshotCount() < before);
    }

    db->reset();
    fs::remove_all(temp_dir);
}

TEST_CASE("count-based retention does not touch another endpoint's snapshots",
          "[cache][ducklake][retention]") {
    // `keep-last-snapshots` is configured PER ENDPOINT, but every cached
    // endpoint shares ONE DuckLake catalog - and the expiry listed snapshots
    // with a catalog-wide `SELECT snapshot_id FROM ducklake_snapshots(cat)`.
    // So endpoint /a refreshing with keep_last=2 expired endpoint /b's
    // snapshots as well: b lost its time-travel history and, because
    // fetchSnapshotInfo reads its watermark from its newest surviving
    // snapshot, its next incremental refresh silently re-read from the wrong
    // point.
    //
    // The existing retention test uses a single table, so it cannot see this.
    // Two tables is the whole point of the test.
    fs::path temp_dir = fs::temp_directory_path() / "flapi_retention_two_tables";
    fs::remove_all(temp_dir);
    fs::create_directories(temp_dir / "data");
    fs::path config_path = temp_dir / "config.yaml";

    {
        std::ofstream cfg(config_path);
        cfg << R"(
project-name: retention_two_tables
project-description: per-endpoint retention must stay per-endpoint

template:
  path: )" << temp_dir.string() << R"(

duckdb:
  db_path: )" << (temp_dir / "rt.db").string() << R"(

ducklake:
  enabled: true
  alias: cache
  metadata-path: )" << (temp_dir / "metadata.ducklake").string() << R"(
  data-path: )" << (temp_dir / "data").string() << R"(

connections:
  default:
    init: "SELECT 1;"
)";
    }

    auto config_manager = std::make_shared<ConfigManager>(config_path);
    config_manager->loadConfig();
    auto db = DatabaseManager::getInstance();
    db->reset();
    REQUIRE_NOTHROW(db->initializeDBManagerFromConfig(config_manager));

    auto adapter = std::make_shared<CapturingRealAdapter>(db);
    CacheManager cache_manager(adapter);

    std::map<std::string, std::string> p;
    db->executeQuery("CREATE SCHEMA IF NOT EXISTS cache.s", p, false);
    db->executeQuery("CREATE TABLE cache.s.a AS SELECT 1 AS i", p, false);
    db->executeQuery("CREATE TABLE cache.s.b AS SELECT 1 AS i", p, false);
    // Interleaved, so b's snapshots sit both above and below a's in the
    // catalog-wide ordering the broken version used.
    for (int i = 2; i <= 6; ++i) {
        db->executeQuery("INSERT INTO cache.s.a VALUES (" + std::to_string(i) + ")", p, false);
        db->executeQuery("INSERT INTO cache.s.b VALUES (" + std::to_string(i) + ")", p, false);
    }

    auto snapshotsOf = [&](const std::string& table) {
        std::map<std::string, std::string> q;
        auto r = db->executeQuery(
            "SELECT count(*) AS n FROM ducklake_snapshots('cache') "
            "WHERE list_contains(flatten(map_values(changes)), 's." + table + "') "
            "   OR list_contains(flatten(map_values(changes)), "
            "      (SELECT CAST(table_id AS VARCHAR) FROM ducklake_table_info('cache') "
            "       WHERE table_name = '" + table + "' LIMIT 1))", q, false);
        auto rows = crow::json::load(r.data.dump());
        return static_cast<int64_t>(rows[0]["n"].d());
    };

    const int64_t a_before = snapshotsOf("a");
    const int64_t b_before = snapshotsOf("b");
    REQUIRE(a_before > 2);
    REQUIRE(b_before > 2);

    // Only /a has a retention policy. /b has none at all.
    auto endpoint_a = cachedEndpoint("/a", "a");
    endpoint_a.cache.retention.keep_last_snapshots = 2;

    std::map<std::string, std::string> params;
    cache_manager.refreshDuckLakeCache(config_manager, endpoint_a, params);

    SECTION("a's own snapshots were expired") {
        // Without this the test would pass by expiring nothing at all.
        REQUIRE(snapshotsOf("a") < a_before);
    }

    SECTION("b's snapshots are untouched") {
        REQUIRE(snapshotsOf("b") == b_before);
    }

    SECTION("the expire call names no snapshot that belongs to b") {
        std::string expire;
        for (const auto& q : adapter->executed) {
            if (q.find("ducklake_expire_snapshots") != std::string::npos) {
                expire = q;
            }
        }
        REQUIRE_FALSE(expire.empty());

        std::map<std::string, std::string> q;
        auto r = db->executeQuery(
            "SELECT snapshot_id FROM ducklake_snapshots('cache') "
            "WHERE list_contains(flatten(map_values(changes)), 's.b') "
            "   OR list_contains(flatten(map_values(changes)), "
            "      (SELECT CAST(table_id AS VARCHAR) FROM ducklake_table_info('cache') "
            "       WHERE table_name = 'b' LIMIT 1))", q, false);
        auto rows = crow::json::load(r.data.dump());
        REQUIRE(rows.size() > 0);
        for (size_t i = 0; i < rows.size(); ++i) {
            const auto id = std::to_string(static_cast<int64_t>(rows[i]["snapshot_id"].d()));
            INFO("expire call: " << expire << " must not name b's snapshot " << id);
            REQUIRE(expire.find(id) == std::string::npos);
        }
    }

    db->reset();
    fs::remove_all(temp_dir);
}
