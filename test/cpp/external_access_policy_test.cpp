#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <filesystem>
#include <string>

#include "config_manager.hpp"
#include "database_manager.hpp"
#include "external_access_policy.hpp"
#include "extension_installer.hpp"
#include "test_utils.hpp"

using namespace flapi;
using namespace flapi::test;
namespace fs = std::filesystem;

namespace {

std::string ConfigWith(const std::string& block) {
    return "project-name: hardening\ntemplate:\n  path: ./sqls\nconnections:\n  inmem:\n    properties:\n"
           "      database: ':memory:'\n" + block;
}

bool Contains(const std::vector<std::string>& items, const std::string& item) {
    return std::find(items.begin(), items.end(), item) != items.end();
}

// Restores the process-wide flag whatever the test did.
struct RestrictedScope {
    explicit RestrictedScope(bool restricted) { SetExternalAccessRestricted(restricted); }
    ~RestrictedScope() { SetExternalAccessRestricted(false); }
};

}  // namespace

TEST_CASE("RefusedTemplateStatement names extension and secret statements", "[external_access]") {
    CHECK(RefusedTemplateStatement("LOAD fts") == std::optional<std::string>("LOAD"));
    CHECK(RefusedTemplateStatement("install spatial from core") == std::optional<std::string>("INSTALL"));
    CHECK(RefusedTemplateStatement("FORCE INSTALL x FROM 'http://h/r'") == std::optional<std::string>("FORCE INSTALL"));
    CHECK(RefusedTemplateStatement("SELECT 1; LOAD httpfs") == std::optional<std::string>("LOAD"));
    CHECK(RefusedTemplateStatement("CREATE SECRET s (TYPE s3, KEY_ID 'a', SECRET 'b')") ==
          std::optional<std::string>("CREATE SECRET"));
    CHECK(RefusedTemplateStatement("CREATE OR REPLACE PERSISTENT SECRET s (TYPE s3)") ==
          std::optional<std::string>("CREATE SECRET"));
    CHECK(RefusedTemplateStatement("DROP SECRET zz_flapi_s3") == std::optional<std::string>("DROP SECRET"));
    CHECK(RefusedTemplateStatement("UPDATE EXTENSIONS") == std::optional<std::string>("UPDATE EXTENSIONS"));
}

TEST_CASE("RefusedTemplateStatement lets ordinary SQL through", "[external_access]") {
    CHECK_FALSE(RefusedTemplateStatement("SELECT * FROM t WHERE id = $1"));
    // The keywords as identifiers or in strings are not statements.
    CHECK_FALSE(RefusedTemplateStatement("SELECT load, secret FROM servers"));
    CHECK_FALSE(RefusedTemplateStatement("SELECT * FROM payload WHERE note = 'please install'"));
    CHECK_FALSE(RefusedTemplateStatement("UPDATE extensions_table SET x = 1"));
    CHECK_FALSE(RefusedTemplateStatement("CREATE TABLE secrets AS SELECT 1"));
}

TEST_CASE("RefusedTemplateStatement fails closed on SQL it cannot parse", "[external_access]") {
    CHECK(RefusedTemplateStatement("LOAD fts; SELECT FROM WHERE (((").has_value());
}

TEST_CASE("RefuseRestrictedTemplateStatements only refuses while restricted", "[external_access]") {
    {
        RestrictedScope scope(false);
        CHECK_NOTHROW(RefuseRestrictedTemplateStatements("LOAD fts"));
    }
    {
        RestrictedScope scope(true);
        try {
            RefuseRestrictedTemplateStatements("LOAD fts");
            FAIL("LOAD was not refused");
        } catch (const std::runtime_error& e) {
            const std::string message = e.what();
            CHECK(message.find("LOAD") != std::string::npos);
            CHECK(message.find("external access") != std::string::npos);
            CHECK(message.find("duckdb-hardening") != std::string::npos);
        }
        CHECK_NOTHROW(RefuseRestrictedTemplateStatements("SELECT 1"));
    }
}

TEST_CASE("ExplainExternalAccessError adds the hint only for extension refusals under the policy",
          "[external_access]") {
    const std::string refusal = "Permission Error: Loading external extensions is disabled through configuration";
    {
        RestrictedScope scope(false);
        CHECK(ExplainExternalAccessError(refusal) == refusal);
    }
    {
        RestrictedScope scope(true);
        CHECK(ExplainExternalAccessError(refusal).find("duckdb.default-extensions") != std::string::npos);
        CHECK(ExplainExternalAccessError("Binder Error: x") == "Binder Error: x");
    }
    CHECK(ExtensionUnavailableUnderHardening("spatial").find("'spatial'") != std::string::npos);
}

TEST_CASE("duckdb-hardening defaults to auto with empty lists", "[external_access][config]") {
    TempTestConfig config(ConfigWith(""), "flapi_hardening_default");
    auto manager = config.createConfigManager();
    const auto& hardening = manager->getDuckDBHardeningConfig();
    CHECK(hardening.external_access == ExternalAccessMode::Auto);
    CHECK(hardening.allowed_directories.empty());
    CHECK(hardening.allowed_paths.empty());
    CHECK_FALSE(manager->isRemoteConfig());
}

TEST_CASE("duckdb-hardening absolutises local entries and keeps URLs", "[external_access][config]") {
    TempTestConfig config(ConfigWith("duckdb-hardening:\n  external-access: restricted\n"
                                     "  allowed-directories: [./data, 's3://other/prefix', /srv/x/]\n"
                                     "  allowed-paths: [./one.csv]\n"),
                          "flapi_hardening_lists");
    auto manager = config.createConfigManager();
    const auto& hardening = manager->getDuckDBHardeningConfig();
    CHECK(hardening.external_access == ExternalAccessMode::Restricted);
    CHECK(Contains(hardening.allowed_directories, (fs::current_path() / "data").string() + "/"));
    CHECK(Contains(hardening.allowed_directories, "s3://other/prefix/"));
    CHECK(Contains(hardening.allowed_directories, "/srv/x/"));
    CHECK(Contains(hardening.allowed_paths, (fs::current_path() / "one.csv").string()));
}

TEST_CASE("duckdb-hardening refuses an unknown mode or key", "[external_access][config]") {
    {
        TempTestConfig config(ConfigWith("duckdb-hardening:\n  external-access: lockdown\n"), "flapi_hardening_bad");
        CHECK_THROWS_WITH(config.createConfigManager(),
                          Catch::Matchers::ContainsSubstring("duckdb-hardening.external-access"));
    }
    {
        TempTestConfig config(ConfigWith("duckdb-hardening:\n  allowed-dirs: [/x]\n"), "flapi_hardening_key");
        CHECK_THROWS_WITH(config.createConfigManager(), Catch::Matchers::ContainsSubstring("allowed-dirs"));
    }
}

TEST_CASE("The allow list covers the config dir, templates and the DuckLake data path", "[external_access]") {
    TempTestConfig config(ConfigWith("ducklake:\n  enabled: true\n  alias: cache\n"
                                     "  metadata-path: ./meta.ducklake\n  data-path: ./lake_data\n"
                                     "duckdb-hardening:\n  allowed-paths: [/etc/hostname]\n"),
                          "flapi_hardening_allow");
    auto manager = config.createConfigManager();
    const auto allow = DatabaseManager::ComputeExternalAccessAllowList(*manager, false);
    const auto dir = fs::path(config.dirPath()).lexically_normal().string();
    CHECK(Contains(allow.directories, dir + (dir.back() == '/' ? "" : "/")));
    CHECK(Contains(allow.directories, fs::path(config.sqlsPath()).lexically_normal().string() + "/"));
    CHECK(Contains(allow.paths, "/etc/hostname"));
    CHECK(std::any_of(allow.directories.begin(), allow.directories.end(), [](const std::string& entry) {
        return entry.size() > 10 && entry.compare(entry.size() - 10, 10, "lake_data/") == 0;
    }));
    CHECK_FALSE(Contains(allow.directories, "embed://"));
    // Never the extension directory: allowed directories are writable.
    for (const auto& entry : allow.directories) {
        CHECK(entry.find(".duckdb/extensions") == std::string::npos);
    }

    const auto bundled = DatabaseManager::ComputeExternalAccessAllowList(*manager, true);
    CHECK(Contains(bundled.directories, "embed://"));
}

// --------------------------------------------------------------------------- #253 review

TEST_CASE("The template allowlist refuses wrapped and out-of-list statements", "[external_access]") {
    for (const char* sql : {
             "EXPLAIN ANALYZE CREATE SECRET s (TYPE s3, KEY_ID 'a', SECRET 'b')",
             "EXPLAIN ANALYZE DROP SECRET zz_flapi_s3",
             "EXPLAIN ANALYZE INSTALL fts FROM core",
             "EXPLAIN LOAD fts",
             "PREPARE p AS CREATE SECRET s (TYPE s3, KEY_ID 'a', SECRET 'b')",
             "PRAGMA import_database('/tmp/x')",
             "IMPORT DATABASE '/tmp/x'",
             "EXPLAIN ANALYZE PRAGMA import_database('/tmp/x')",
             "EXPORT DATABASE '/tmp/x'",
             "ATTACH '/tmp/x.duckdb' AS x",
             "DETACH x",
             "COPY FROM DATABASE a TO b",
             "PRAGMA enable_profiling",
             "SET GLOBAL TimeZone = 'UTC'",
             "RESET GLOBAL TimeZone",
             "SET default_order = 'desc'",
             "SELECT 1; SET GLOBAL threads = 1",
         }) {
        INFO(sql);
        CHECK(RefusedTemplateStatement(sql).has_value());
    }
}

TEST_CASE("The template allowlist keeps what templates use", "[external_access]") {
    for (const char* sql : {
             "SELECT * FROM t WHERE id = $1",
             "WITH a AS (SELECT 1) SELECT * FROM a",
             "INSERT INTO t VALUES (1)",
             "UPDATE t SET x = 1 WHERE id = 2",
             "DELETE FROM t WHERE id = 2",
             "MERGE INTO t USING s ON t.id = s.id WHEN MATCHED THEN UPDATE SET x = s.x",
             "CREATE OR REPLACE TABLE cache.main.t AS SELECT 1",
             "CREATE TABLE IF NOT EXISTS t (x INTEGER)",
             "CREATE OR REPLACE VIEW v AS SELECT 1",
             "CREATE MACRO m(a) AS a + 1",
             "CREATE SCHEMA IF NOT EXISTS s",
             "DROP TABLE IF EXISTS t",
             "DROP VIEW IF EXISTS v",
             "ALTER TABLE t ADD COLUMN y INTEGER",
             "COPY (SELECT 1) TO '/tmp/x.csv'",
             "COPY t FROM '/tmp/x.csv'",
             "BEGIN TRANSACTION; SELECT 1; COMMIT",
             "ROLLBACK",
             "SET SESSION TimeZone = 'UTC'",
             "SET schema = 'main'",
             "USE main",
             "SET VARIABLE x = 1",
             "CHECKPOINT",
             "CALL ducklake_expire_snapshots('cache', dry_run => true)",
             "VACUUM",
             "EXPLAIN SELECT 1",
             "EXPLAIN ANALYZE SELECT 1",
             "PREPARE q AS SELECT 1",
             "EXECUTE q",
             "DESCRIBE t",
             "SHOW TABLES",
         }) {
        INFO(sql);
        CHECK_FALSE(RefusedTemplateStatement(sql).has_value());
    }
}

TEST_CASE("LoadOrInstallExtension decides on the connection, not the process flag", "[external_access]") {
    RestrictedScope scope(true);   // as if the server's own DuckDB were locked down
    duckdb_database db = nullptr;
    duckdb_connection conn = nullptr;
    REQUIRE(duckdb_open(nullptr, &db) == DuckDBSuccess);
    REQUIRE(duckdb_connect(db, &conn) == DuckDBSuccess);
    // An unrestricted instance (the bootstrap DB, a private one) must get DuckDB's own answer.
    const auto error = LoadOrInstallExtension(conn, "no_such_extension_253", /*offline=*/true);
    CHECK_FALSE(error.empty());
    CHECK(error.find("duckdb-hardening") == std::string::npos);

    duckdb_result result;
    REQUIRE(duckdb_query(conn, "SET enable_external_access = false", &result) == DuckDBSuccess);
    duckdb_destroy_result(&result);
    const auto restricted_error = LoadOrInstallExtension(conn, "no_such_extension_253", /*offline=*/true);
    CHECK(restricted_error.find("duckdb-hardening") != std::string::npos);
    duckdb_disconnect(&conn);
    duckdb_close(&db);
}

TEST_CASE("The preload covers aws for credential_chain and azure wherever it is named", "[external_access]") {
    TempTestConfig config(
        "project-name: preload\ntemplate:\n  path: ./sqls\nconnections:\n"
        "  s3conn:\n    init: \"CREATE SECRET s (TYPE s3, PROVIDER credential_chain)\"\n"
        "    properties:\n      database: ':memory:'\n"
        "  azconn:\n    properties:\n      path: 'az://container/data.parquet'\n",
        "flapi_hardening_preload");
    auto manager = config.createConfigManager();
    const auto preload = DatabaseManager::ComputeExternalAccessPreload(*manager);
    CHECK(Contains(preload, "httpfs"));
    CHECK(Contains(preload, "aws"));
    CHECK(Contains(preload, "azure"));
}

TEST_CASE("The allow list carries the CA bundle and strips file://", "[external_access]") {
    const std::string bundle = "/etc/ssl/certs/ca-certificates.crt";
    TempTestConfig config(ConfigWith("duckdb-hardening:\n  allowed-directories: ['file:///srv/data']\n"),
                          "flapi_hardening_ca");
    auto manager = config.createConfigManager();
    const auto allow = DatabaseManager::ComputeExternalAccessAllowList(*manager, false);
    CHECK(Contains(allow.directories, "/srv/data/"));
    if (fs::exists(bundle)) {
        CHECK(Contains(allow.paths, bundle));
    }
}
