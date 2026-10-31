#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include "archive_io.hpp"
#include "config_manager.hpp"
#include "extension_installer.hpp"
#include "pack.hpp"
#include "test_utils.hpp"

#ifdef _WIN32
#define setenv(name, value, overwrite) _putenv_s(name, value)
#define unsetenv(name) _putenv((std::string(name) + "=").c_str())
#endif

using namespace flapi;
using namespace flapi::test;
namespace fs = std::filesystem;

namespace {

std::vector<std::uint8_t> bytesOf(const std::string& s) {
    return std::vector<std::uint8_t>(s.begin(), s.end());
}

std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

ArchiveEntries sampleBundle() {
    ArchiveEntries entries;
    entries["flapi.yaml"] = bytesOf("project-name: x\n");
    entries[std::string(kBundledExtensionsPrefix) + "v1.5.5/linux_amd64/httpfs.duckdb_extension"] = bytesOf("ELF-httpfs");
    entries[std::string(kBundledExtensionsPrefix) + "v1.5.5/linux_amd64/core_functions.duckdb_extension"] = bytesOf("ELF-core");
    return entries;
}

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

// Resets the process-wide bundled directory around a test.
class BundledDirGuard {
public:
    explicit BundledDirGuard(std::optional<fs::path> dir) { SetBundledExtensionDirectory(std::move(dir)); }
    ~BundledDirGuard() { SetBundledExtensionDirectory(std::nullopt); }
};

const std::string kConfig = R"(
project-name: bundled-ext
project-description: Bundled extensions
http-port: 8080
template:
  path: ./sqls
connections:
  test:
    properties:
      path: ./data.parquet
)";

}  // namespace

TEST_CASE("MaterializeBundledExtensions writes only extension entries, once", "[extensions][bundle]") {
    TempDirectory cache("bundled_ext_cache");
    auto entries = sampleBundle();

    auto dir = MaterializeBundledExtensions(entries, cache.path());
    REQUIRE(dir.has_value());
    REQUIRE(readFile(*dir / "v1.5.5/linux_amd64/httpfs.duckdb_extension") == "ELF-httpfs");
    REQUIRE(readFile(*dir / "v1.5.5/linux_amd64/core_functions.duckdb_extension") == "ELF-core");
    REQUIRE_FALSE(fs::exists(*dir / "flapi.yaml"));

    // Same contents, same directory: a restart reuses it rather than rewriting.
    const auto mtime = fs::last_write_time(*dir / "v1.5.5/linux_amd64/httpfs.duckdb_extension");
    auto again = MaterializeBundledExtensions(entries, cache.path());
    REQUIRE(again == dir);
    REQUIRE(fs::last_write_time(*again / "v1.5.5/linux_amd64/httpfs.duckdb_extension") == mtime);

    // Different contents, different directory: a re-packed binary never sees stale files.
    entries[std::string(kBundledExtensionsPrefix) + "v1.5.5/linux_amd64/httpfs.duckdb_extension"] = bytesOf("ELF-httpfs-v2");
    auto changed = MaterializeBundledExtensions(entries, cache.path());
    REQUIRE(changed.has_value());
    REQUIRE(*changed != *dir);
}

TEST_CASE("MaterializeBundledExtensions returns nothing for a bundle without extensions", "[extensions][bundle]") {
    TempDirectory cache("bundled_ext_none");
    ArchiveEntries entries;
    entries["flapi.yaml"] = bytesOf("project-name: x\n");
    REQUIRE_FALSE(MaterializeBundledExtensions(entries, cache.path()).has_value());
}

TEST_CASE("MaterializeBundledExtensions refuses entries that escape the cache", "[extensions][bundle]") {
    TempDirectory cache("bundled_ext_escape");
    ArchiveEntries entries;
    entries[std::string(kBundledExtensionsPrefix) + "../../evil.duckdb_extension"] = bytesOf("x");
    REQUIRE_THROWS(MaterializeBundledExtensions(entries, cache.path()));
}

TEST_CASE("A bundled extension directory is the default, and implies offline", "[extensions][bundle]") {
    EnvGuard dir_env("FLAPI_EXTENSION_DIRECTORY", std::nullopt);
    EnvGuard off_env("FLAPI_EXTENSIONS_OFFLINE", std::nullopt);
    BundledDirGuard bundled(fs::path("/bundled/ext"));

    SECTION("bundle only") {
        TempTestConfig temp(kConfig, "bundled_default");
        auto cm = temp.createConfigManager();
        REQUIRE(cm->getDuckDBConfig().settings.at("extension_directory") == "/bundled/ext");
        REQUIRE(cm->getDuckDBConfig().offline_extensions);
    }
    SECTION("FLAPI_EXTENSIONS_OFFLINE=0 turns offline off again") {
        EnvGuard off("FLAPI_EXTENSIONS_OFFLINE", std::string("0"));
        TempTestConfig temp(kConfig, "bundled_online");
        REQUIRE_FALSE(temp.createConfigManager()->getDuckDBConfig().offline_extensions);
    }
    SECTION("FLAPI_EXTENSION_DIRECTORY wins over the bundle") {
        EnvGuard dir("FLAPI_EXTENSION_DIRECTORY", std::string("/env/ext"));
        TempTestConfig temp(kConfig, "bundled_env");
        REQUIRE(temp.createConfigManager()->getDuckDBConfig().settings.at("extension_directory") == "/env/ext");
    }
    SECTION("the config wins over both") {
        EnvGuard dir("FLAPI_EXTENSION_DIRECTORY", std::string("/env/ext"));
        TempTestConfig temp(kConfig + "duckdb:\n  extension_directory: /cfg/ext\n", "bundled_cfg");
        REQUIRE(temp.createConfigManager()->getDuckDBConfig().settings.at("extension_directory") == "/cfg/ext");
    }
}

TEST_CASE("Without a bundle nothing changes", "[extensions][bundle]") {
    EnvGuard dir_env("FLAPI_EXTENSION_DIRECTORY", std::nullopt);
    EnvGuard off_env("FLAPI_EXTENSIONS_OFFLINE", std::nullopt);
    BundledDirGuard none(std::nullopt);
    TempTestConfig temp(kConfig, "bundled_absent");
    auto cm = temp.createConfigManager();
    REQUIRE(cm->getDuckDBConfig().settings.count("extension_directory") == 0);
    REQUIRE_FALSE(cm->getDuckDBConfig().offline_extensions);
}
