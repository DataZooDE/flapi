#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include "config_manager.hpp"
#include "database_manager.hpp"

using namespace flapi;
namespace fs = std::filesystem;

namespace {
// Runs on every exit path (including a failed REQUIRE): the singleton is shut
// down and reset so later test cases start from an uninitialized manager, then
// the temp directory is removed.
struct DuckLakeTestTeardown {
    fs::path temp_dir;
    std::shared_ptr<DatabaseManager> db_manager;

    ~DuckLakeTestTeardown() {
        if (db_manager) {
            db_manager->shutdown();
            db_manager->reset();
        }
        std::error_code ec;
        fs::remove_all(temp_dir, ec);
    }
};
} // namespace

TEST_CASE("DatabaseManager attaches DuckLake catalog when enabled", "[database_manager][ducklake]") {
    fs::path temp_dir = fs::temp_directory_path() / "flapi_ducklake_test";
    fs::remove_all(temp_dir);
    fs::create_directories(temp_dir);
    fs::path db_path = temp_dir / "ducklake_test.db";
    fs::path metadata_path = temp_dir / "metadata.ducklake";
    fs::path data_path = temp_dir / "cache_data";
    fs::create_directories(data_path);

    fs::path templates_dir = temp_dir / "templates";
    fs::create_directories(templates_dir);

    std::ofstream template_file(templates_dir / "dummy.sql");
    template_file << "SELECT 1";
    template_file.close();

    fs::path config_path = temp_dir / "config.yaml";
    {
        std::ofstream config_file(config_path);
        config_file << R"(
project-name: ducklake_test
project-description: DuckLake attach test

template:
  path: )" << templates_dir.string() << R"(

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

    auto db_manager = DatabaseManager::getInstance();
    DuckLakeTestTeardown teardown{temp_dir, db_manager};
    REQUIRE_NOTHROW(db_manager->initializeDBManagerFromConfig(config_manager));

    std::map<std::string, std::string> params;
    // DuckLake catalogs expose no information_schema; ask the engine which
    // databases are attached instead. The configured alias must be one of them.
    auto attached = db_manager->executeQuery(
        "SELECT count(*) AS cnt FROM duckdb_databases() WHERE database_name = 'cache'", params, false);
    REQUIRE(attached.data.size() == 1);
    CHECK(attached.data[0]["cnt"].dump() == "1");

    // And it is queryable: a schema can be created and listed through the alias.
    REQUIRE_NOTHROW(db_manager->executeQuery("CREATE SCHEMA IF NOT EXISTS cache.probe", params, false));
    auto schemas = db_manager->executeQuery(
        "SELECT count(*) AS cnt FROM duckdb_schemas() WHERE database_name = 'cache' AND schema_name = 'probe'",
        params, false);
    REQUIRE(schemas.data.size() == 1);
    CHECK(schemas.data[0]["cnt"].dump() == "1");
}
