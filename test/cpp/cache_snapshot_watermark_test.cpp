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

TEST_CASE("the incremental watermark does not skip rows written during a refresh",
          "[cache][ducklake][watermark]") {
    // previousSnapshotTimestamp was ducklake_snapshots.snapshot_time - the
    // instant the previous refresh COMMITTED. But that refresh read the
    // source at some earlier instant T_read. A source row written at T with
    //
    //     T_read < T <= T_commit
    //
    // was not read by refresh N, and the next refresh's
    // `WHERE updated_at > TIMESTAMP '<T_commit>'` excludes it too. On a
    // continuously-written source every refresh permanently drops the rows
    // written during its own execution and reports success - invisible under
    // both append and merge.
    //
    // The watermark therefore comes from the DATA: max(cursor) over what is
    // actually cached. Rows above it are exactly the rows not yet loaded.
    //
    // This test writes to the source DURING the window the previous refresh
    // was running, which is the condition the snapshot-time version cannot
    // survive.
    fs::path temp_dir = fs::temp_directory_path() / "flapi_watermark_race";
    fs::remove_all(temp_dir);
    fs::create_directories(temp_dir / "data");
    fs::path config_path = temp_dir / "config.yaml";

    {
        std::ofstream cfg(config_path);
        cfg << R"(
project-name: watermark_race
project-description: a refresh must not skip rows written while it ran

template:
  path: )" << temp_dir.string() << R"(

duckdb:
  db_path: )" << (temp_dir / "wr.db").string() << R"(

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
    // The cache holds rows up to updated_at = 100. That is what the previous
    // refresh actually loaded.
    db->executeQuery(
        "CREATE TABLE cache.s.c AS "
        "SELECT * FROM (VALUES (1, 50), (2, 100)) AS t(id, updated_at)", p, false);

    // ...and the snapshot committing that load lands LATER than rows written
    // while it ran. Row 3 at updated_at = 150 is such a row: written after
    // the refresh read the source, before it committed.
    //
    // Anything keyed on the commit instant skips row 3 forever. The data
    // watermark is 100, so row 3 is above it and gets loaded.

    auto endpoint = cachedEndpoint("/c", "c");
    endpoint.cache.cursor = CacheConfig::CursorConfig{};
    endpoint.cache.cursor->column = "updated_at";
    endpoint.cache.cursor->type = "int";

    std::map<std::string, std::string> params;
    cache_manager.refreshDuckLakeCache(config_manager, endpoint, params);

    const auto it = adapter->captured_params.find("previousSnapshotTimestamp");
    REQUIRE(it != adapter->captured_params.end());

    SECTION("the watermark is the largest cursor value actually cached") {
        REQUIRE(it->second == "100");
    }

    SECTION("it is not the snapshot commit time") {
        // The defect, stated directly. A commit timestamp is a date-time
        // string; the cursor here is an integer, so the two cannot be
        // confused.
        INFO("watermark was: " << it->second);
        REQUIRE(it->second.find('-') == std::string::npos);
        REQUIRE(it->second.find(':') == std::string::npos);
    }

    SECTION("the watermark admits the late row and excludes the cached one") {
        // Once the watermark is pinned to "100" above, running
        // `WHERE updated_at > 100` against rows the TEST wrote is arithmetic,
        // not a property of the product - the exact "asserts against SQL it
        // wrote itself" pattern this file's header says a review caught once
        // already. What is worth stating is the RELATION between the
        // watermark and the data that produced it, which is the thing the
        // commit-time version got wrong.
        // The property, against the DATA rather than against constants the
        // test wrote: the watermark equals max(cursor) over what is actually
        // cached. Re-derived from the table, so it fails if the product picks
        // any other value - including the commit timestamp it used to use.
        std::map<std::string, std::string> q;
        auto r = db->executeQuery(
            "SELECT CAST(max(updated_at) AS VARCHAR) AS m FROM cache.s.c", q, false);
        auto rows = crow::json::load(r.data.dump());
        REQUIRE(it->second == rows[0]["m"].s());
    }

    db->reset();
    fs::remove_all(temp_dir);
}

namespace {

// A DuckLake catalog with two cached tables, for the retention cases that a
// single-table fixture structurally cannot see.
struct TwoTableCatalog {
    fs::path dir;
    std::shared_ptr<ConfigManager> config;
    std::shared_ptr<DatabaseManager> db;
    std::shared_ptr<CapturingRealAdapter> adapter;

    explicit TwoTableCatalog(const std::string& name) {
        dir = fs::temp_directory_path() / name;
        fs::remove_all(dir);
        fs::create_directories(dir / "data");
        {
            std::ofstream cfg(dir / "config.yaml");
            cfg << "\nproject-name: " << name
                << "\nproject-description: retention must stay per endpoint\n\n"
                << "template:\n  path: " << dir.string() << "\n\n"
                << "duckdb:\n  db_path: " << (dir / "t.db").string() << "\n\n"
                << "ducklake:\n  enabled: true\n  alias: cache\n"
                << "  metadata-path: " << (dir / "metadata.ducklake").string() << "\n"
                << "  data-path: " << (dir / "data").string() << "\n\n"
                << "connections:\n  default:\n    init: \"SELECT 1;\"\n";
        }
        config = std::make_shared<ConfigManager>(dir / "config.yaml");
        config->loadConfig();
        db = DatabaseManager::getInstance();
        db->reset();
        REQUIRE_NOTHROW(db->initializeDBManagerFromConfig(config));
        adapter = std::make_shared<CapturingRealAdapter>(db);
    }

    void sql(const std::string& q) {
        std::map<std::string, std::string> p;
        db->executeQuery(q, p, false);
    }

    int64_t snapshotsOf(const std::string& schema, const std::string& table) {
        std::map<std::string, std::string> p;
        auto r = db->executeQuery(
            "SELECT count(*) AS n FROM ducklake_snapshots('cache') "
            "WHERE list_contains(flatten(map_values(changes)), '" + schema + "." + table + "') "
            "   OR list_contains(flatten(map_values(changes)), "
            "      (SELECT CAST(table_id AS VARCHAR) FROM ducklake_table_info('cache') "
            "       WHERE table_name = '" + table + "' LIMIT 1))", p, false);
        auto rows = crow::json::load(r.data.dump());
        return static_cast<int64_t>(rows[0]["n"].d());
    }

    ~TwoTableCatalog() {
        db->reset();
        fs::remove_all(dir);
    }
};

}  // namespace

TEST_CASE("age-based retention does not touch another endpoint's snapshots",
          "[cache][ducklake][retention]") {
    // `max-snapshot-age` emitted
    //     CALL ducklake_expire_snapshots(cat, older_than => <ts>)
    // which has NO per-table form - it acts on the whole catalog, and every
    // cached endpoint shares one. The count-based branch beside it was fixed
    // for exactly this; this branch was left, which is the failure mode the
    // round exists to break. It now resolves explicit per-table version ids
    // like the count branch does.
    TwoTableCatalog cat("flapi_retention_age");
    CacheManager cache_manager(cat.adapter);

    cat.sql("CREATE SCHEMA IF NOT EXISTS cache.s");
    cat.sql("CREATE TABLE cache.s.a AS SELECT 1 AS i");
    cat.sql("CREATE TABLE cache.s.b AS SELECT 1 AS i");
    for (int i = 2; i <= 5; ++i) {
        cat.sql("INSERT INTO cache.s.a VALUES (" + std::to_string(i) + ")");
        cat.sql("INSERT INTO cache.s.b VALUES (" + std::to_string(i) + ")");
    }
    const int64_t a_before = cat.snapshotsOf("s", "a");
    const int64_t b_before = cat.snapshotsOf("s", "b");
    REQUIRE(a_before > 2);
    REQUIRE(b_before > 2);

    auto endpoint_a = cachedEndpoint("/a", "a");
    // Everything already written is older than "now", so every one of a's own
    // snapshots is eligible.
    endpoint_a.cache.retention.max_snapshot_age = "0 seconds";

    std::map<std::string, std::string> params;
    cache_manager.refreshDuckLakeCache(cat.config, endpoint_a, params);

    SECTION("the expiry is by explicit versions, never catalog-wide older_than") {
        std::string expire;
        for (const auto& q : cat.adapter->executed) {
            if (q.find("ducklake_expire_snapshots") != std::string::npos) {
                expire = q;
            }
        }
        REQUIRE_FALSE(expire.empty());
        REQUIRE(expire.find("versions") != std::string::npos);
        INFO("expire call: " << expire);
        REQUIRE(expire.find("older_than") == std::string::npos);
    }

    SECTION("a's own snapshots ARE expired") {
        // Without this the test passes when the expire CALL throws - the
        // failure is caught and logged at WARNING - so it would assert only
        // that b was unharmed by an expiry that never happened. The
        // count-based sibling has this assertion; this one was written
        // without it.
        REQUIRE(cat.snapshotsOf("s", "a") < a_before);
    }

    SECTION("but a keeps its newest snapshot, so the watermark survives") {
        // Age-based retention with no keep_last used to make every matching
        // snapshot a candidate, the current one included - and
        // fetchSnapshotInfo reads the incremental watermark from the newest
        // surviving snapshot.
        REQUIRE(cat.snapshotsOf("s", "a") >= 1);
    }

    SECTION("b's snapshots are untouched") {
        REQUIRE(cat.snapshotsOf("s", "b") == b_before);
    }
}

TEST_CASE("manual GC obeys the endpoint's own retention, per table",
          "[cache][ducklake][retention]") {
    // performGarbageCollection ran a catalog-wide older_than expiry against a
    // HARDCODED one-day cutoff, ignoring the configured policy entirely. One
    // `flapii cache gc` therefore destroyed every other endpoint's history.
    TwoTableCatalog cat("flapi_retention_gc");
    CacheManager cache_manager(cat.adapter);

    cat.sql("CREATE SCHEMA IF NOT EXISTS cache.s");
    cat.sql("CREATE TABLE cache.s.a AS SELECT 1 AS i");
    cat.sql("CREATE TABLE cache.s.b AS SELECT 1 AS i");
    for (int i = 2; i <= 5; ++i) {
        cat.sql("INSERT INTO cache.s.a VALUES (" + std::to_string(i) + ")");
        cat.sql("INSERT INTO cache.s.b VALUES (" + std::to_string(i) + ")");
    }
    const int64_t a_before = cat.snapshotsOf("s", "a");
    const int64_t b_before = cat.snapshotsOf("s", "b");

    auto endpoint_a = cachedEndpoint("/a", "a");
    endpoint_a.cache.retention.keep_last_snapshots = 2;
    cache_manager.performGarbageCollection(cat.config, endpoint_a, {});

    SECTION("a's own snapshots are expired down to the configured count") {
        REQUIRE(cat.snapshotsOf("s", "a") < a_before);
    }

    SECTION("b's snapshots are untouched") {
        REQUIRE(cat.snapshotsOf("s", "b") == b_before);
    }

    SECTION("the hardcoded one-day catalog-wide cutoff is gone") {
        for (const auto& q : cat.adapter->executed) {
            if (q.find("ducklake_expire_snapshots") != std::string::npos) {
                INFO("expire call: " << q);
                REQUIRE(q.find("INTERVAL '1 day'") == std::string::npos);
                REQUIRE(q.find("older_than") == std::string::npos);
            }
        }
    }
}

TEST_CASE("a snapshot shared with another table is never expired",
          "[cache][ducklake][retention]") {
    // The exclusivity half of the per-table fix, which both single-table
    // tests pass trivially. A snapshot can carry changes for several tables,
    // and ducklake_expire_snapshots discards all of them - so a shared
    // snapshot must be retained even though that means keeping more history
    // than `keep-last-snapshots` asks for.
    TwoTableCatalog cat("flapi_retention_shared");
    CacheManager cache_manager(cat.adapter);

    cat.sql("CREATE SCHEMA IF NOT EXISTS cache.s");
    cat.sql("CREATE TABLE cache.s.a AS SELECT 1 AS i");
    cat.sql("CREATE TABLE cache.s.b AS SELECT 1 AS i");
    // One transaction writing BOTH tables: a single snapshot naming both.
    // Issued as ONE statement - separate executeQuery calls do not share a
    // connection, so BEGIN/COMMIT across them produces two snapshots and the
    // test would silently stop exercising the shared case.
    //
    // Verified shape on DuckDB 1.5.5:
    //   {inlined_insert=[2, 3]}   <- both table ids, one snapshot
    cat.sql("BEGIN TRANSACTION; "
            "INSERT INTO cache.s.a VALUES (2); "
            "INSERT INTO cache.s.b VALUES (2); "
            "COMMIT;");
    std::map<std::string, std::string> p;
    auto shared = cat.db->executeQuery(
        "SELECT count(*) AS n FROM ducklake_snapshots('cache') "
        "WHERE list_contains(flatten(map_values(changes)), "
        "        (SELECT CAST(table_id AS VARCHAR) FROM ducklake_table_info('cache') "
        "         WHERE table_name = 'a' LIMIT 1)) "
        "  AND list_contains(flatten(map_values(changes)), "
        "        (SELECT CAST(table_id AS VARCHAR) FROM ducklake_table_info('cache') "
        "         WHERE table_name = 'b' LIMIT 1))", p, false);
    auto shared_rows = crow::json::load(shared.data.dump());
    const int64_t shared_count = static_cast<int64_t>(shared_rows[0]["n"].d());
    // If DuckLake ever stops coalescing this into one snapshot, the test is
    // no longer exercising what it claims to.
    REQUIRE(shared_count >= 1);

    const int64_t b_before = cat.snapshotsOf("s", "b");

    auto endpoint_a = cachedEndpoint("/a", "a");
    endpoint_a.cache.retention.keep_last_snapshots = 1;
    std::map<std::string, std::string> params;
    cache_manager.refreshDuckLakeCache(cat.config, endpoint_a, params);

    SECTION("b keeps every snapshot it had") {
        REQUIRE(cat.snapshotsOf("s", "b") == b_before);
    }

    SECTION("no expire call names a snapshot that also touched b") {
        std::string expire;
        for (const auto& q : cat.adapter->executed) {
            if (q.find("ducklake_expire_snapshots") != std::string::npos) {
                expire = q;
            }
        }
        // Not `if (!expire.empty())`: a run that expires nothing at all would
        // satisfy every assertion below vacuously, which is the pattern this
        // file exists to avoid. Either an expiry happened and must exclude
        // b's snapshots, or the shared-snapshot guard held and there is
        // nothing to expire - and the section above pins which.
        {
            auto r = cat.db->executeQuery(
                "SELECT snapshot_id FROM ducklake_snapshots('cache') "
                "WHERE list_contains(flatten(map_values(changes)), "
                "        (SELECT CAST(table_id AS VARCHAR) FROM ducklake_table_info('cache') "
                "         WHERE table_name = 'b' LIMIT 1))", p, false);
            auto rows = crow::json::load(r.data.dump());
            for (size_t i = 0; i < rows.size(); ++i) {
                const auto id = std::to_string(static_cast<int64_t>(rows[i]["snapshot_id"].d()));
                INFO("expire: " << expire << " must not name b's snapshot " << id);
                REQUIRE(expire.find(id) == std::string::npos);
            }
        }
    }
}

TEST_CASE("retention is disabled when two schemas hold the same table name",
          "[cache][ducklake][retention]") {
    // ducklake_table_info() exposes table_name and schema_id but no schema
    // NAME, so `WHERE table_name = 'orders' LIMIT 1` picks an arbitrary one of
    // s1.orders and s2.orders. That fed a read-only watermark query before; it
    // now feeds snapshot expiry, where a wrong id deletes another endpoint's
    // data. Ambiguity must therefore expire NOTHING.
    TwoTableCatalog cat("flapi_retention_ambiguous");
    CacheManager cache_manager(cat.adapter);

    cat.sql("CREATE SCHEMA IF NOT EXISTS cache.s1");
    cat.sql("CREATE SCHEMA IF NOT EXISTS cache.s2");
    cat.sql("CREATE TABLE cache.s1.orders AS SELECT 1 AS i");
    cat.sql("CREATE TABLE cache.s2.orders AS SELECT 1 AS i");
    for (int i = 2; i <= 6; ++i) {
        cat.sql("INSERT INTO cache.s1.orders VALUES (" + std::to_string(i) + ")");
        cat.sql("INSERT INTO cache.s2.orders VALUES (" + std::to_string(i) + ")");
    }

    std::map<std::string, std::string> p;
    auto total_before = cat.db->executeQuery(
        "SELECT count(*) AS n FROM ducklake_snapshots('cache')", p, false);
    const int64_t before =
        static_cast<int64_t>(crow::json::load(total_before.data.dump())[0]["n"].d());

    auto endpoint = cachedEndpoint("/orders", "orders");
    endpoint.cache.schema = "s2";
    endpoint.cache.retention.keep_last_snapshots = 1;
    std::map<std::string, std::string> params;
    cache_manager.refreshDuckLakeCache(cat.config, endpoint, params);

    SECTION("nothing is expired at all") {
        auto total_after = cat.db->executeQuery(
            "SELECT count(*) AS n FROM ducklake_snapshots('cache')", p, false);
        const int64_t after =
            static_cast<int64_t>(crow::json::load(total_after.data.dump())[0]["n"].d());
        REQUIRE(after == before);
    }

    SECTION("no expire call is issued") {
        for (const auto& q : cat.adapter->executed) {
            INFO("query: " << q);
            REQUIRE(q.find("ducklake_expire_snapshots") == std::string::npos);
        }
    }
}

TEST_CASE("an unavailable cursor watermark falls back to a full load, not to a timestamp",
          "[cache][ducklake][watermark]") {
    // When a cursor is configured but max(cursor) cannot be read - the cache
    // table was created and is still empty, or the cursor column is not in it
    // - previousSnapshotTimestamp used to keep the snapshot COMMIT time. The
    // two values do not share a type: an int cursor renders
    // `WHERE seq > 2026-09-23 10:00:00`, a binder error; a timestamp cursor
    // renders `> '<table create time>'` and drops every older source row
    // forever. Erasing it makes the template take its full-load branch, which
    // is always correct and merely slower.
    TwoTableCatalog cat("flapi_watermark_fallback");
    CacheManager cache_manager(cat.adapter);

    cat.sql("CREATE SCHEMA IF NOT EXISTS cache.s");
    // Created, committed, and EMPTY.
    cat.sql("CREATE TABLE cache.s.e (id INTEGER, seq INTEGER)");

    auto endpoint = cachedEndpoint("/e", "e");
    endpoint.cache.cursor = CacheConfig::CursorConfig{};
    endpoint.cache.cursor->column = "seq";
    endpoint.cache.cursor->type = "int";

    std::map<std::string, std::string> params;
    cache_manager.refreshDuckLakeCache(cat.config, endpoint, params);

    SECTION("previousSnapshotTimestamp is absent, so the full-load branch renders") {
        REQUIRE(cat.adapter->captured_params.find("previousSnapshotTimestamp") ==
                cat.adapter->captured_params.end());
    }

    SECTION("and it is certainly not a timestamp") {
        const auto it = cat.adapter->captured_params.find("previousSnapshotTimestamp");
        if (it != cat.adapter->captured_params.end()) {
            INFO("watermark was: " << it->second);
            REQUIRE(it->second.find(':') == std::string::npos);
        }
    }
}
