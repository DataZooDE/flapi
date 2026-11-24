#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <filesystem>
#include <string>

#include "config_manager.hpp"
#include "database_manager.hpp"
#include "external_access_policy.hpp"
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
