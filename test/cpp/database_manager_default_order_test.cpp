#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include "config_manager.hpp"
#include "database_manager.hpp"

using namespace flapi;
namespace fs = std::filesystem;

// Regression (#211): `duckdb.default_order: DESC` was set globally. DuckLake runs
// its catalog queries on its own connection, and reads a table's columns with a
// bare `ORDER BY ... column_order`, so under DESC it read them reversed and every
// commit into the catalog failed with "Could not convert string '...' to INT64" -
// every cache build and every audit event of examples/flapi-test.yaml.
TEST_CASE("DatabaseManager: default_order DESC does not break DuckLake commits", "[database_manager][ducklake]") {
    fs::path temp_dir = fs::temp_directory_path() / "flapi_ducklake_default_order_test";
    fs::remove_all(temp_dir);
    fs::create_directories(temp_dir / "cache_data");
    fs::create_directories(temp_dir / "templates");

    fs::path config_path = temp_dir / "config.yaml";
    {
        std::ofstream config_file(config_path);
        config_file << R"(
project-name: ducklake_default_order_test
project-description: default_order with DuckLake
template:
  path: )" << (temp_dir / "templates").string() << R"(
duckdb:
  default_order: DESC
ducklake:
  enabled: true
  alias: cache
  metadata-path: )" << (temp_dir / "metadata.ducklake").string() << R"(
  data-path: )" << (temp_dir / "cache_data").string() << R"(
connections:
  default:
    init: "SELECT 1;"
)";
    }

    auto config_manager = std::make_shared<ConfigManager>(config_path);
    config_manager->loadConfig();
    auto db_manager = DatabaseManager::getInstance();
    REQUIRE_NOTHROW(db_manager->initializeDBManagerFromConfig(config_manager));

    std::map<std::string, std::string> params;
    // The audit table's shape: VARCHARs before BIGINT/TIMESTAMP columns.
    REQUIRE_NOTHROW(db_manager->executeQuery(
        "CREATE SCHEMA IF NOT EXISTS cache.audit", params, false));
    REQUIRE_NOTHROW(db_manager->executeQuery(
        "CREATE TABLE cache.audit.events (id VARCHAR, schema_name VARCHAR, rows_affected BIGINT, ts TIMESTAMP)",
        params, false));
    REQUIRE_NOTHROW(db_manager->executeQuery(
        "INSERT INTO cache.audit.events VALUES ('e1', 'analytics', 5, TIMESTAMP '2026-01-01 00:00:00')",
        params, false));
    auto rows = db_manager->executeQuery("SELECT schema_name, rows_affected FROM cache.audit.events", params, false);
    REQUIRE(rows.data.size() == 1);
    CHECK(rows.data[0]["schema_name"].dump() == "\"analytics\"");

    // The configured order still applies to flapi's own queries.
    auto ordered = db_manager->executeQuery("SELECT i FROM range(3) t(i) ORDER BY i", params, false);
    REQUIRE(ordered.data.size() == 3);
    CHECK(ordered.data[0]["i"].dump() == "2");

    db_manager.reset();
    config_manager.reset();
    fs::remove_all(temp_dir);
}
