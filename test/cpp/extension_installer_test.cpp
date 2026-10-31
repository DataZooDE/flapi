#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include "config_manager.hpp"
#include "database_manager.hpp"
#include "extension_installer.hpp"
#include "test_utils.hpp"

using namespace flapi;
using namespace flapi::test;
namespace fs = std::filesystem;

namespace {

// Sets an environment variable for the lifetime of the guard and restores it after.
class EnvGuard {
public:
    EnvGuard(const char* name, const std::optional<std::string>& value) : _name(name) {
        if (const char* old = std::getenv(name)) {
            _old = old;
        }
        if (value) {
            setenv(name, value->c_str(), 1);
        } else {
            unsetenv(name);
        }
    }
    ~EnvGuard() {
        if (_old) {
            setenv(_name.c_str(), _old->c_str(), 1);
        } else {
            unsetenv(_name.c_str());
        }
    }

private:
    std::string _name;
    std::optional<std::string> _old;
};

std::string baseConfig(const std::string& duckdb_block, const std::string& connections_block = "") {
    return R"(
project-name: ext-test
project-description: Extension test
http-port: 8080
template:
  path: ./sqls
)" + duckdb_block + R"(
connections:
  test:
    properties:
      path: ./data.parquet
)" + connections_block;
}

}  // namespace

TEST_CASE("duckdb.default-extensions keeps today's list when absent", "[extensions]") {
    EnvGuard dir("FLAPI_EXTENSION_DIRECTORY", std::nullopt);
    TempTestConfig temp(baseConfig(""), "ext_default");
    auto cm = temp.createConfigManager();
    const auto& ext = cm->getDuckDBConfig().default_extensions;
    REQUIRE(ext == std::vector<std::string>{"httpfs", "ducklake", "fts", "json", "postgres", "sqlite", "parquet"});
}

TEST_CASE("duckdb.default-extensions is configurable, and not passed to DuckDB as a setting", "[extensions]") {
    SECTION("a list") {
        TempTestConfig temp(baseConfig("duckdb:\n  default-extensions: [json, parquet]\n  threads: 2\n"), "ext_list");
        auto cm = temp.createConfigManager();
        REQUIRE(cm->getDuckDBConfig().default_extensions == std::vector<std::string>{"json", "parquet"});
        REQUIRE(cm->getDuckDBConfig().settings.count("default-extensions") == 0);
        REQUIRE(cm->getDuckDBConfig().settings.at("threads") == "2");
    }
    SECTION("an empty list") {
        TempTestConfig temp(baseConfig("duckdb:\n  default-extensions: []\n"), "ext_empty");
        auto cm = temp.createConfigManager();
        REQUIRE(cm->getDuckDBConfig().default_extensions.empty());
    }
    SECTION("a scalar is refused") {
        TempTestConfig temp(baseConfig("duckdb:\n  default-extensions: json\n"), "ext_scalar");
        REQUIRE_THROWS(temp.createConfigManager());
    }
}

TEST_CASE("FLAPI_EXTENSION_DIRECTORY sets extension_directory unless the config does", "[extensions]") {
    EnvGuard dir("FLAPI_EXTENSION_DIRECTORY", std::string("/opt/flapi/extensions"));
    SECTION("env fills the gap") {
        TempTestConfig temp(baseConfig(""), "ext_env_dir");
        auto cm = temp.createConfigManager();
        REQUIRE(cm->getDuckDBConfig().settings.at("extension_directory") == "/opt/flapi/extensions");
    }
    SECTION("the config wins") {
        TempTestConfig temp(baseConfig("duckdb:\n  extension_directory: /custom\n"), "ext_cfg_dir");
        auto cm = temp.createConfigManager();
        REQUIRE(cm->getDuckDBConfig().settings.at("extension_directory") == "/custom");
    }
}

TEST_CASE("FLAPI_EXTENSIONS_OFFLINE turns on offline extensions", "[extensions]") {
    {
        EnvGuard off("FLAPI_EXTENSIONS_OFFLINE", std::nullopt);
        TempTestConfig temp(baseConfig(""), "ext_online");
        REQUIRE_FALSE(temp.createConfigManager()->getDuckDBConfig().offline_extensions);
    }
    for (const std::string value : {"1", "true", "yes"}) {
        EnvGuard off("FLAPI_EXTENSIONS_OFFLINE", value);
        TempTestConfig temp(baseConfig(""), "ext_offline");
        REQUIRE(temp.createConfigManager()->getDuckDBConfig().offline_extensions);
    }
}

TEST_CASE("ParseExtensionSpec reads name[@repository]", "[extensions]") {
    auto core = ParseExtensionSpec("httpfs");
    REQUIRE(core.has_value());
    REQUIRE(core->name == "httpfs");
    REQUIRE(core->repository.empty());

    auto community = ParseExtensionSpec("bigquery@community");
    REQUIRE(community.has_value());
    REQUIRE(community->name == "bigquery");
    REQUIRE(community->repository == "community");

    // Names are interpolated into SQL: anything but identifiers is refused.
    REQUIRE_FALSE(ParseExtensionSpec("").has_value());
    REQUIRE_FALSE(ParseExtensionSpec("json; DROP TABLE x").has_value());
    REQUIRE_FALSE(ParseExtensionSpec("json@").has_value());
    REQUIRE_FALSE(ParseExtensionSpec("json@comm'unity").has_value());
}

TEST_CASE("BuildInstallStatement quotes the name and names the repository", "[extensions]") {
    REQUIRE(BuildInstallStatement({"httpfs", ""}) == "INSTALL \"httpfs\";");
    REQUIRE(BuildInstallStatement({"bigquery", "community"}) == "INSTALL \"bigquery\" FROM community;");
}

TEST_CASE("ExtractInstallSpecs finds INSTALL statements in connection init SQL", "[extensions]") {
    const std::string init = R"(
        INSTALL 'bigquery' FROM community;
        LOAD 'bigquery';
        install postgres;
        FORCE INSTALL "spatial";
        SELECT 'INSTALL nothing';
    )";
    auto specs = ExtractInstallSpecs(init);
    REQUIRE(specs.size() == 3);
    REQUIRE(specs[0].name == "bigquery");
    REQUIRE(specs[0].repository == "community");
    REQUIRE(specs[1].name == "postgres");
    REQUIRE(specs[1].repository.empty());
    REQUIRE(specs[2].name == "spatial");
}

TEST_CASE("CollectExtensionSpecs merges defaults and connection inits without duplicates", "[extensions]") {
    TempTestConfig temp(baseConfig("duckdb:\n  default-extensions: [json, postgres]\n",
                                   "  bq:\n    init: |\n      INSTALL 'bigquery' FROM community;\n      INSTALL postgres;\n"),
                        "ext_collect");
    auto cm = temp.createConfigManager();
    auto specs = CollectExtensionSpecs(*cm);
    std::vector<std::string> names;
    for (const auto& spec : specs) {
        names.push_back(spec.repository.empty() ? spec.name : spec.name + "@" + spec.repository);
    }
    // core_functions first: flAPI's DuckDB loads no extension statically, and every query
    // needs it, so a pre-installed set without it still downloads on the first query.
    REQUIRE(names == std::vector<std::string>{"core_functions", "json", "postgres", "bigquery@community"});
}

TEST_CASE("CollectExtensionSpecs always includes the extensions flAPI itself needs", "[extensions]") {
    TempTestConfig temp(baseConfig("duckdb:\n  default-extensions: []\n"), "ext_collect_implicit");
    auto cm = temp.createConfigManager();
    auto specs = CollectExtensionSpecs(*cm);
    REQUIRE(specs.size() == 1);
    REQUIRE(specs.front().name == "core_functions");
}

TEST_CASE("Offline mode fails fast on a missing extension and names the fix", "[extensions][database_manager]") {
    // An empty extension directory: nothing may be found in ~/.duckdb from earlier tests.
    TempDirectory empty_dir("ext_offline_empty_dir");
    EnvGuard dir("FLAPI_EXTENSION_DIRECTORY", empty_dir.path());
    TempTestConfig temp(baseConfig("duckdb:\n  default-extensions: [flapi_test_missing_ext]\n"), "ext_offline_missing");
    EnvGuard off("FLAPI_EXTENSIONS_OFFLINE", std::string("1"));
    auto cm = temp.createConfigManager();
    auto db = DatabaseManager::getInstance();
    db->reset();
    try {
        db->initializeDBManagerFromConfig(cm);
        db->reset();
        FAIL("offline start with a missing extension must throw");
    } catch (const std::exception& ex) {
        const std::string what = ex.what();
        INFO(what);
        REQUIRE(what.find("flapi_test_missing_ext") != std::string::npos);
        REQUIRE(what.find("core_functions") != std::string::npos);
        REQUIRE(what.find("flapi extensions install") != std::string::npos);
    }
    db->reset();
}

TEST_CASE("Online mode still only warns about an extension it cannot load", "[extensions][database_manager]") {
    EnvGuard off("FLAPI_EXTENSIONS_OFFLINE", std::nullopt);
    TempTestConfig temp(baseConfig("duckdb:\n  default-extensions: [flapi_test_missing_ext]\n"), "ext_online_missing");
    auto cm = temp.createConfigManager();
    auto db = DatabaseManager::getInstance();
    db->reset();
    REQUIRE_NOTHROW(db->initializeDBManagerFromConfig(cm));
    db->reset();
}

TEST_CASE("InstallExtensions reports an extension it cannot install", "[extensions]") {
    // An unknown extension is a reported failure, not an exception, so a build step can print
    // every problem at once.
    TempDirectory dir("ext_install_unknown");
    std::ostringstream log;
    const int failures = InstallExtensions({{"flapi_test_missing_ext", ""}}, dir.path(), log);
    REQUIRE(failures == 1);
    REQUIRE(log.str().find("flapi_test_missing_ext") != std::string::npos);
}
