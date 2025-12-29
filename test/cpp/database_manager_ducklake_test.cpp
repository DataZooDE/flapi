#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include "config_manager.hpp"
#include "database_manager.hpp"

using namespace flapi;
namespace fs = std::filesystem;

TEST_CASE("DatabaseManager attaches DuckLake catalog when enabled", "[database_manager][ducklake]") {
    fs::path temp_dir = fs::temp_directory_path() / "flapi_ducklake_test";
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
server-name: test_server

template:
  path: )" << templates_dir.string() << R"(

duckdb:
  db_path: )" << db_path.string() << R"(

ducklake:
  enabled: true
  alias: cache
  metadata_path: )" << metadata_path.string() << R"(
  data_path: )" << data_path.string() << R"(

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
    REQUIRE_NOTHROW(db_manager->executeQuery("SELECT * FROM cache.information_schema.tables LIMIT 1", params, false));

    db_manager.reset();
    config_manager.reset();
    fs::remove_all(temp_dir);
}
