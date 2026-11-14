// Cluster F1 (#211): read-only replicas, credential redaction, catalog
// directories, embed:// outside a bundle, template.path normalisation.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <crow.h>
#define private public
#include "../../src/include/cache_manager.hpp"
#undef private
#include "cache_refresh_command.hpp"
#include "config_manager.hpp"
#include "database_manager.hpp"
#include "duckdb_embed_fs.hpp"
#include "test_utils.hpp"

using namespace flapi;
using namespace flapi::test;

namespace {

class TableListingAdapter : public ICacheDatabaseAdapter {
public:
    std::set<std::pair<std::string, std::string>> existing_tables;  // (schema, table)
    int listings = 0;

    std::string renderCacheTemplate(const EndpointConfig&, const CacheConfig&,
                                    std::map<std::string, std::string>&) override {
        return "SELECT 1";
    }
    void executeDuckLakeQuery(const std::string&, const std::map<std::string, std::string>&) override {}
    QueryResult executeDuckLakeQueryWithResult(const std::string& query) override {
        QueryResult result;
        std::vector<crow::json::wvalue> rows;
        if (query.find("duckdb_tables()") != std::string::npos) {
            ++listings;
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

std::string ducklakeConfig(const std::string& extra) {
    return R"(
project-name: f1-test
project-description: cluster F1
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

void writeCachedEndpoint(TempTestConfig& temp, const std::string& name) {
    temp.writeEndpoint(name + ".yaml", "url-path: /" + name + "\n"
                       "method: GET\n"
                       "template-source: " + name + ".sql\n"
                       "connection: [test]\n"
                       "cache:\n"
                       "  enabled: true\n"
                       "  schema: main\n"
                       "  table: " + name + "_cache\n");
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

TEST_CASE("a read-only replica notices a cache the writer built", "[cache_manager][read_only][f1]") {
    // A replica started before the writer built a table answered 503
    // cache_not_built FOREVER: Unbuilt lived in memory and only a local refresh -
    // which a replica cannot run - cleared it.
    TempTestConfig temp(ducklakeConfig("  read-only: true\n"), "f1_replica_recheck");
    writeCachedEndpoint(temp, "orders");
    auto cm = temp.createConfigManager();
    auto adapter = std::make_shared<TableListingAdapter>();
    CacheManager cache_manager(adapter);

    cache_manager.warmUpCaches(cm);   // read-only: warm-up is `never`
    const auto& endpoint = endpointFor(cm, "orders_cache");
    REQUIRE(cache_manager.getEndpointReadiness(cm, endpoint).state == CacheManager::ReadinessState::Unbuilt);

    SECTION("on the request path, throttled") {
        REQUIRE(cache_manager.readinessBlock(cm, endpoint).has_value());   // still missing
        adapter->existing_tables.insert({"main", "orders_cache"});        // the writer builds it
        const int listings = adapter->listings;
        // Within the throttle window the catalog is not listed again.
        REQUIRE(cache_manager.readinessBlock(cm, endpoint).has_value());
        REQUIRE(adapter->listings == listings);
        // Past it, the table is found and served.
        for (auto& [key, at] : cache_manager.readonly_rechecked_at_) {
            at -= CacheManager::kReadOnlyRecheckInterval + std::chrono::seconds(1);
        }
        REQUIRE_FALSE(cache_manager.readinessBlock(cm, endpoint).has_value());
        REQUIRE(cache_manager.getEndpointReadiness(cm, endpoint).state == CacheManager::ReadinessState::Ready);
    }

    SECTION("on the heartbeat, forced") {
        REQUIRE(cache_manager.recheckReadOnlyCaches(cm, /*force=*/true) == 0);
        adapter->existing_tables.insert({"main", "orders_cache"});
        REQUIRE(cache_manager.recheckReadOnlyCaches(cm, /*force=*/true) == 1);
        REQUIRE(cache_manager.getEndpointReadiness(cm, endpoint).state == CacheManager::ReadinessState::Ready);
    }
}

TEST_CASE("a writer does not re-check readiness behind its own refreshes", "[cache_manager][read_only][f1]") {
    TempTestConfig temp(ducklakeConfig("  warmup: never\n"), "f1_writer_no_recheck");
    writeCachedEndpoint(temp, "orders");
    auto cm = temp.createConfigManager();
    auto adapter = std::make_shared<TableListingAdapter>();
    CacheManager cache_manager(adapter);
    cache_manager.warmUpCaches(cm);
    adapter->existing_tables.insert({"main", "orders_cache"});
    REQUIRE(cache_manager.recheckReadOnlyCaches(cm, /*force=*/true) == 0);
    REQUIRE(cache_manager.readinessBlock(cm, endpointFor(cm, "orders_cache")).has_value());
}

TEST_CASE("connection-string credentials are redacted", "[security][redaction][f1]") {
    REQUIRE(RedactConnectionSecrets("postgres:dbname=lake host=db user=flapi password=s3cr3t port=5432") ==
            "postgres:dbname=lake host=db user=flapi password=<redacted> port=5432");
    REQUIRE(RedactConnectionSecrets("postgres:dbname=lake password='with space' host=db") ==
            "postgres:dbname=lake password=<redacted> host=db");
    REQUIRE(RedactConnectionSecrets("mysql:db=x;PWD=abc;user=u") == "mysql:db=x;PWD=<redacted>;user=u");
    REQUIRE(RedactConnectionSecrets("PGPASSWORD=abc psql") == "PGPASSWORD=<redacted> psql");
    REQUIRE(RedactConnectionSecrets("postgres://flapi:s3cr3t@db:5432/lake") == "postgres://flapi:<redacted>@db:5432/lake");
    REQUIRE(RedactConnectionSecrets("https://bucket/x?token=abc&v=1") == "https://bucket/x?token=<redacted>&v=1");
    // In a quoted ATTACH statement, the closing quote stays.
    REQUIRE(RedactConnectionSecrets("ATTACH 'ducklake:postgres:dbname=l password=pw' AS cache") ==
            "ATTACH 'ducklake:postgres:dbname=l password=<redacted>' AS cache");
    // Nothing secret: unchanged.
    REQUIRE(RedactConnectionSecrets("/var/lib/flapi/cache.ducklake") == "/var/lib/flapi/cache.ducklake");
    REQUIRE(RedactConnectionSecrets("s3://bucket/cache/") == "s3://bucket/cache/");
}

TEST_CASE("GET /_config/project redacts the DuckLake DSN and reports the effective warm-up",
          "[security][redaction][f1]") {
    TempTestConfig temp(R"(
project-name: f1-test
project-description: cluster F1
template:
  path: ./sqls
ducklake:
  enabled: true
  alias: cache
  metadata-path: "postgres:dbname=lake host=db user=flapi password=hunter2"
  data-path: ./cache_data
  read-only: true
  warmup: always
connections:
  test:
    properties:
      path: ./data.parquet
)", "f1_project_redaction");
    auto cm = temp.createConfigManager();
    const std::string json = cm->getFlapiConfig().dump();
    REQUIRE(json.find("hunter2") == std::string::npos);
    REQUIRE(json.find("password=<redacted>") != std::string::npos);
    REQUIRE(json.find("\"warmup\":\"always\"") != std::string::npos);
    REQUIRE(json.find("\"warmup_effective\":\"never\"") != std::string::npos);
}

TEST_CASE("a DuckLake attach failure is redacted and a lock conflict explained", "[security][ducklake][f1]") {
    DuckLakeConfig ducklake;
    ducklake.metadata_path = "postgres:dbname=lake password=hunter2";
    const std::string raw = "Failed to prepare statement: ATTACH 'ducklake:postgres:dbname=lake password=hunter2' AS cache";
    const auto message = DatabaseManager::DuckLakeAttachErrorMessage(ducklake, raw);
    REQUIRE(message.find("hunter2") == std::string::npos);
    REQUIRE(message.rfind("Failed to attach DuckLake catalog: ", 0) == 0);

    DuckLakeConfig file;
    file.metadata_path = "/srv/cache.ducklake";
    const auto locked = DatabaseManager::DuckLakeAttachErrorMessage(
        file, "IO Error: Could not set lock on file \"/srv/cache.ducklake\": Conflicting lock is held in flapi (PID 7)");
    REQUIRE(locked.find("stop the other flapi") != std::string::npos);
    REQUIRE(locked.find("Postgres/SQLite/MySQL") != std::string::npos);
    REQUIRE(IsCatalogLockError(locked));   // the one-shot still recognises it

    REQUIRE(CatalogLockMessage("/x").find("stop the replicas") != std::string::npos);
}

TEST_CASE("DuckLake directories are created for a writer and checked for a replica", "[ducklake][f1]") {
    TempDirectory dir("f1_ducklake_dirs");
    DuckLakeConfig ducklake;
    ducklake.metadata_path = (std::filesystem::path(dir.path()) / "meta" / "deep" / "cache.ducklake").string();
    ducklake.data_path = (std::filesystem::path(dir.path()) / "data" / "lake").string();

    SECTION("a replica does not create them, and says which is missing") {
        ducklake.read_only = true;
        const auto problem = DatabaseManager::PrepareDuckLakeDirectories(ducklake);
        REQUIRE(problem.has_value());
        REQUIRE(problem->find("does not exist") != std::string::npos);
        REQUIRE_FALSE(std::filesystem::exists(std::filesystem::path(dir.path()) / "meta"));
    }

    SECTION("a writer creates both") {
        REQUIRE_FALSE(DatabaseManager::PrepareDuckLakeDirectories(ducklake).has_value());
        REQUIRE(std::filesystem::is_directory(std::filesystem::path(dir.path()) / "meta" / "deep"));
        REQUIRE(std::filesystem::is_directory(ducklake.data_path));
    }

    SECTION("a Postgres catalog and an object store need nothing") {
        ducklake.metadata_path = "postgres:dbname=lake host=db";
        ducklake.data_path = "s3://bucket/lake/";
        ducklake.read_only = true;
        REQUIRE_FALSE(DatabaseManager::PrepareDuckLakeDirectories(ducklake).has_value());
    }
}

TEST_CASE("embed:// outside a bundle resolves inside the configuration directory", "[embed][f1]") {
    TempDirectory dir("f1_embed_dir");
    DirectoryEmbedFileSystem fs(dir.path());
    const auto root = std::filesystem::weakly_canonical(dir.path());
    REQUIRE(fs.CanHandleFile("embed://data/x.csv"));
    REQUIRE_FALSE(fs.CanHandleFile("/data/x.csv"));
    REQUIRE(fs.MapPath("embed://data/x.csv") == (root / "data" / "x.csv").string());
    REQUIRE(fs.MapPath("embed://./data/../data/x.csv") == (root / "data" / "x.csv").string());
    REQUIRE_THROWS(fs.MapPath("embed://../outside.csv"));
    REQUIRE_THROWS(fs.MapPath("embed://data/../../outside.csv"));
    REQUIRE_THROWS(fs.MapPath("embed:///etc/passwd"));
}

TEST_CASE("ducklake.compaction.delete-threshold is a fraction", "[config_manager][compaction][f1]") {
    SECTION("accepted") {
        TempTestConfig temp(ducklakeConfig("  compaction:\n    enabled: true\n    delete-threshold: 0.25\n"),
                            "f1_threshold_ok");
        auto cm = temp.createConfigManager();
        REQUIRE(cm->getDuckLakeConfig().compaction.delete_threshold == 0.25);
        REQUIRE(cm->getFlapiConfig().dump().find("\"delete-threshold\":0.25") != std::string::npos);
    }
    SECTION("out of range is refused at load") {
        TempTestConfig temp(ducklakeConfig("  compaction:\n    enabled: true\n    delete-threshold: 1.5\n"),
                            "f1_threshold_bad");
        REQUIRE_THROWS(temp.createConfigManager());
    }
}

TEST_CASE("template.path ../sqls is not path traversal", "[config_manager][validation][f1]") {
    // `template.path: ../sqls` flagged every template under it as traversal and
    // then claimed the existing files did not exist.
    TempDirectory dir("f1_template_parent");
    const std::filesystem::path root(dir.path());
    std::filesystem::create_directories(root / "conf");
    std::filesystem::create_directories(root / "sqls");
    {
        std::ofstream(root / "conf" / "flapi.yaml") << R"(
project-name: f1
project-description: f1
template:
  path: ../sqls
connections:
  src:
    properties:
      path: ./x.csv
)";
        std::ofstream(root / "sqls" / "a.yaml") << "url-path: /a\nmethod: GET\ntemplate-source: a.sql\nconnection: [src]\n";
        std::ofstream(root / "sqls" / "a.sql") << "SELECT 1 AS x";
    }
    auto cm = std::make_shared<ConfigManager>(root / "conf" / "flapi.yaml");
    cm->loadConfig();
    REQUIRE(cm->getTemplatePath().find("..") == std::string::npos);
    const auto endpoint = cm->getEndpointForPath("/a");
    REQUIRE(endpoint != nullptr);
    const auto result = cm->validateEndpointConfig(*endpoint);
    for (const auto& warning : result.warnings) {
        INFO("warning: " << warning);
        REQUIRE(warning.find("traversal") == std::string::npos);
        REQUIRE(warning.find("does not exist") == std::string::npos);
        REQUIRE(warning.find("escapes") == std::string::npos);
    }
}
