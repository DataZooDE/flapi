#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include <crow.h>
#include "cache_refresh_command.hpp"
#include "config_manager.hpp"
#include "config_service.hpp"
#include "database_manager.hpp"
#include "heartbeat_worker.hpp"

using namespace flapi;
namespace fs = std::filesystem;

namespace {

// One DuckLake file catalog shared by a writer configuration and a reader configuration,
// as a refresh job and a Cloud Run replica would share one on a bucket.
class SharedCatalog {
public:
    explicit SharedCatalog(const std::string& prefix)
        : _dir(fs::temp_directory_path() / (prefix + "_" + std::to_string(::getpid()))) {
        fs::remove_all(_dir);
        fs::create_directories(_dir / "sqls");
        fs::create_directories(_dir / "cache_data");
        write("sqls/nums.yaml", R"(
url-path: /nums
method: GET
template-source: nums.sql
connection: [local]
cache:
  enabled: true
  schema: main
  table: nums_cache
  template-file: nums_cache.sql
)");
        write("sqls/nums.sql", "SELECT * FROM cache.main.nums_cache");
        write("sqls/nums_cache.sql",
              "CREATE OR REPLACE TABLE {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS "
              "SELECT * FROM range(3) t(x)");
        write("sqls/broken.yaml", R"(
url-path: /broken
method: GET
template-source: broken.sql
connection: [local]
cache:
  enabled: true
  schema: main
  table: broken_cache
  template-file: broken_cache.sql
)");
        write("sqls/broken.sql", "SELECT 1");
        write("sqls/broken_cache.sql", "SELECT * FROM table_that_does_not_exist");
    }

    ~SharedCatalog() {
        auto db = DatabaseManager::getInstance();
        db->reset();
        std::error_code ec;
        fs::remove_all(_dir, ec);
    }

    std::shared_ptr<ConfigManager> config(const std::string& extra_ducklake) {
        const std::string name = "flapi_" + std::to_string(++_configs) + ".yaml";
        write(name, R"(
project-name: reader-test
project-description: Shared DuckLake reader test
http-port: 8080
template:
  path: ./sqls
duckdb:
  default-extensions: [ducklake]
ducklake:
  enabled: true
  alias: cache
  metadata-path: )" + (_dir / "catalog.ducklake").string() + R"(
  data-path: )" + (_dir / "cache_data").string() + R"(
)" + extra_ducklake + R"(
connections:
  local:
    properties:
      path: ./data.parquet
)");
        auto cm = std::make_shared<ConfigManager>(_dir / name);
        cm->loadConfig();
        return cm;
    }

private:
    void write(const std::string& rel, const std::string& content) {
        std::ofstream(_dir / rel) << content;
    }

    fs::path _dir;
    int _configs = 0;
};

int64_t countRows(const std::string& table) {
    auto db = DatabaseManager::getInstance();
    auto result = db->executeQuery("SELECT count(*) AS n FROM " + table, {}, false);
    auto json = crow::json::load(result.data.dump());
    REQUIRE(json);
    REQUIRE(json.size() == 1);
    return json[0]["n"].i();
}

}  // namespace

TEST_CASE("flapi cache refresh: refreshes the selected caches once and exits", "[cache_refresh_command][ducklake]") {
    SharedCatalog catalog("flapi_refresh_cmd");
    auto db = DatabaseManager::getInstance();
    db->reset();

    SECTION("one endpoint: exit 0, summary names the table, rows are in the catalog") {
        auto cm = catalog.config("");
        std::ostringstream out;
        CacheRefreshOptions options;
        options.endpoints = {"/nums"};
        REQUIRE(RunCacheRefreshCommand(cm, options, out) == 0);
        INFO(out.str());
        REQUIRE(out.str().find("\"nums_cache\"") != std::string::npos);
        REQUIRE(out.str().find("\"ok\"") != std::string::npos);
        REQUIRE(countRows("cache.main.nums_cache") == 3);
    }

    SECTION("--all with a failing cache: exit 1, the good one is still refreshed") {
        auto cm = catalog.config("");
        std::ostringstream out;
        CacheRefreshOptions options;
        options.all = true;
        REQUIRE(RunCacheRefreshCommand(cm, options, out) == 1);
        INFO(out.str());
        REQUIRE(out.str().find("\"broken_cache\"") != std::string::npos);
        REQUIRE(out.str().find("\"failed\"") != std::string::npos);
        REQUIRE(countRows("cache.main.nums_cache") == 3);
    }

    SECTION("an unknown endpoint is a usage error, and nothing runs") {
        auto cm = catalog.config("");
        std::ostringstream out;
        CacheRefreshOptions options;
        options.endpoints = {"/nope"};
        REQUIRE(RunCacheRefreshCommand(cm, options, out) == 2);
    }

    SECTION("no selection is a usage error") {
        auto cm = catalog.config("");
        std::ostringstream out;
        REQUIRE(RunCacheRefreshCommand(cm, CacheRefreshOptions{}, out) == 2);
    }

    SECTION("a read-only configuration refuses to refresh") {
        auto cm = catalog.config("  read-only: true\n");
        std::ostringstream out;
        CacheRefreshOptions options;
        options.all = true;
        REQUIRE(RunCacheRefreshCommand(cm, options, out) == 2);
        REQUIRE(out.str().find("read-only") != std::string::npos);
    }
}

TEST_CASE("ducklake.read-only serves another process's cache and cannot change it", "[ducklake][read_only]") {
    SharedCatalog catalog("flapi_ducklake_reader");
    auto db = DatabaseManager::getInstance();
    db->reset();

    // Writer: a one-shot refresh builds the cache, then releases the catalog.
    {
        auto writer = catalog.config("");
        std::ostringstream out;
        CacheRefreshOptions options;
        options.endpoints = {"/nums"};
        REQUIRE(RunCacheRefreshCommand(writer, options, out) == 0);
        db->reset();
    }

    auto reader = catalog.config("  read-only: true\n  warmup: always\n");
    REQUIRE(reader->getDuckLakeConfig().read_only);

    SECTION("read-only forces warm-up off whatever else is configured") {
        reader->setCacheWarmupOverride(CacheWarmupMode::Always);
        auto endpoint = reader->getEndpointForPath("/nums");
        REQUIRE(endpoint != nullptr);
        REQUIRE(reader->getEffectiveCacheWarmupMode(*endpoint) == CacheWarmupMode::Never);
    }

    SECTION("the reader sees the rows and every write is refused") {
        REQUIRE_NOTHROW(db->initializeDBManagerFromConfig(reader));
        REQUIRE(countRows("cache.main.nums_cache") == 3);
        REQUIRE_THROWS(db->executeQuery("INSERT INTO cache.main.nums_cache VALUES (42)", {}, false));

        auto cache_manager = db->getCacheManager();
        REQUIRE(cache_manager);
        cache_manager->warmUpCaches(reader);
        auto nums = reader->getEndpointForPath("/nums");
        auto broken = reader->getEndpointForPath("/broken");
        REQUIRE(cache_manager->getEndpointReadiness(reader, *nums).state == CacheManager::ReadinessState::Ready);
        REQUIRE(cache_manager->getEndpointReadiness(reader, *broken).state == CacheManager::ReadinessState::Unbuilt);
    }

    SECTION("the config service refuses cache mutations with 409") {
        CacheConfigHandler handler(reader);
        crow::request req;
        REQUIRE(handler.refreshCache(req, "/nums").code == 409);
        REQUIRE(handler.performGarbageCollection(req, "/nums").code == 409);
        req.body = R"({"enabled":true,"table":"other","schema":"main"})";
        REQUIRE(handler.updateCacheConfig(req, "/nums").code == 409);
    }

    SECTION("the heartbeat does no cache maintenance on a reader") {
        REQUIRE_FALSE(HeartbeatWorker::ShouldRunCacheMaintenance(reader->getDuckLakeConfig()));
        auto writer = catalog.config("  scheduler:\n    enabled: true\n");
        REQUIRE(HeartbeatWorker::ShouldRunCacheMaintenance(writer->getDuckLakeConfig()));
    }
}

TEST_CASE("ducklake.read-only defaults to false", "[ducklake][read_only]") {
    SharedCatalog catalog("flapi_ducklake_rw_default");
    REQUIRE_FALSE(catalog.config("")->getDuckLakeConfig().read_only);
}
