#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <map>
#include <string>

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
// {{cache.previousSnapshotTimestamp}} drives the incremental WHERE clause - so
// every row changed in between was skipped, and the refresh reported success.
//
// Demonstrated on a real catalog with two cached tables refreshed a, b, a:
// snapshots touching `a` are [4, 2]; the catalog-wide query returned [4, 3],
// and 3 belongs to `b`.
//
// This drives real DuckLake through DatabaseManager rather than calling the
// private fetchSnapshotInfo, so it pins the observable property rather than
// the implementation.
namespace {

std::string perTableSnapshots(const std::string& catalog, const std::string& schema,
                              const std::string& table) {
    const std::string table_id_expr =
        "(SELECT CAST(table_id AS VARCHAR) FROM ducklake_table_info('" + catalog +
        "') WHERE table_name = '" + table + "' LIMIT 1)";
    return "SELECT snapshot_id FROM ducklake_snapshots('" + catalog + "') "
           "WHERE list_contains(flatten(map_values(changes)), " + table_id_expr + ") "
           "   OR list_contains(flatten(map_values(changes)), '" + schema + "." + table + "') "
           "ORDER BY snapshot_id DESC LIMIT 2";
}

}  // namespace

TEST_CASE("the incremental watermark is per table, not per catalog",
          "[cache][ducklake][watermark]") {
    fs::path temp_dir = fs::temp_directory_path() / "flapi_watermark_test";
    fs::remove_all(temp_dir);
    fs::create_directories(temp_dir);
    fs::path config_path = temp_dir / "config.yaml";
    fs::path db_path = temp_dir / "wm.db";
    fs::path metadata_path = temp_dir / "metadata.ducklake";
    fs::path data_path = temp_dir / "data";
    fs::create_directories(data_path);

    {
        std::ofstream cfg(config_path);
        cfg << R"(
project-name: watermark_test
project-description: per-table snapshot watermark

template:
  path: )" << temp_dir.string() << R"(

duckdb:
  db_path: )" << db_path.string() << R"(

ducklake:
  enabled: true
  alias: cache
  metadata-path: )" << metadata_path.string() << R"(
  data-path: )" << data_path.string() << R"(

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

    std::map<std::string, std::string> p;
    db->executeQuery("CREATE SCHEMA IF NOT EXISTS cache.s", p, false);
    // Two cached tables, refreshed interleaved: a, b, then a again.
    db->executeQuery("CREATE TABLE cache.s.a AS SELECT 1 AS i", p, false);
    db->executeQuery("CREATE TABLE cache.s.b AS SELECT 1 AS i", p, false);
    db->executeQuery("INSERT INTO cache.s.a VALUES (2)", p, false);

    auto ids = [&](const std::string& sql) {
        auto r = db->executeQuery(sql, p, false);
        auto rows = crow::json::load(r.data.dump());
        std::vector<int64_t> out;
        if (rows && rows.t() == crow::json::type::List) {
            for (size_t i = 0; i < rows.size(); ++i) {
                out.push_back(static_cast<int64_t>(rows[i]["snapshot_id"].d()));
            }
        }
        return out;
    };

    const auto catalog_wide = ids(
        "SELECT snapshot_id FROM ducklake_snapshots('cache') ORDER BY snapshot_id DESC LIMIT 2");
    const auto for_a = ids(perTableSnapshots("cache", "s", "a"));
    const auto for_b = ids(perTableSnapshots("cache", "s", "b"));

    REQUIRE(for_a.size() == 2);
    REQUIRE(catalog_wide.size() == 2);

    SECTION("a table's previous snapshot is its own, not another table's") {
        // The property that makes incremental refresh correct.
        REQUIRE(for_a[1] != catalog_wide[1]);
        REQUIRE(for_a[1] < catalog_wide[1]);   // the old answer was too NEW, hence skipped rows
    }

    SECTION("a table refreshed once has no previous snapshot") {
        // b was created and never refreshed again. Reporting a previous
        // snapshot for it would make its first incremental refresh skip
        // everything before some unrelated table's commit.
        REQUIRE(for_b.size() == 1);
    }

    SECTION("a table absent from the catalog yields nothing rather than throwing") {
        REQUIRE(ids(perTableSnapshots("cache", "s", "does_not_exist")).empty());
    }

    db->reset();
    fs::remove_all(temp_dir);
}
