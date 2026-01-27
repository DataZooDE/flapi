#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_all.hpp>
#include "vfs_health_checker.hpp"
#include <filesystem>
#include <fstream>

using namespace flapi;

// Helper to create temporary test files
class TempTestFile {
public:
    explicit TempTestFile(const std::string& content = "",
                          const std::string& extension = ".yaml") {
        path_ = std::filesystem::temp_directory_path() /
                ("vfs_health_test_" + std::to_string(reinterpret_cast<uintptr_t>(this)) + extension);
        std::ofstream file(path_);
        file << content;
    }

    ~TempTestFile() {
        if (std::filesystem::exists(path_)) {
            std::filesystem::remove(path_);
        }
    }

    std::filesystem::path path() const { return path_; }
    std::string pathString() const { return path_.string(); }

private:
    std::filesystem::path path_;
};

// Helper to create temporary test directories
class TempTestDir {
public:
    TempTestDir() {
        path_ = std::filesystem::temp_directory_path() /
                ("vfs_health_test_dir_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        std::filesystem::create_directories(path_);
    }

    ~TempTestDir() {
        if (std::filesystem::exists(path_)) {
            std::filesystem::remove_all(path_);
        }
    }

    std::filesystem::path path() const { return path_; }
    std::string pathString() const { return path_.string(); }

    void createFile(const std::string& name, const std::string& content = "") {
        std::ofstream file(path_ / name);
        file << content;
    }

private:
    std::filesystem::path path_;
};

// ============================================================================
// VFSHealthChecker::getSchemeType Tests
// ============================================================================

TEST_CASE("VFSHealthChecker::getSchemeType", "[vfs][health]") {
    SECTION("Local paths return 'local'") {
        REQUIRE(VFSHealthChecker::getSchemeType("/local/path") == "local");
        REQUIRE(VFSHealthChecker::getSchemeType("./relative/path") == "local");
        REQUIRE(VFSHealthChecker::getSchemeType("path.yaml") == "local");
    }

    SECTION("S3 paths return 's3'") {
        REQUIRE(VFSHealthChecker::getSchemeType("s3://bucket/key") == "s3");
        REQUIRE(VFSHealthChecker::getSchemeType("S3://bucket/key") == "s3");
    }

    SECTION("GCS paths return 'gs'") {
        REQUIRE(VFSHealthChecker::getSchemeType("gs://bucket/key") == "gs");
        REQUIRE(VFSHealthChecker::getSchemeType("GS://bucket/key") == "gs");
    }

    SECTION("Azure paths return 'az'") {
        REQUIRE(VFSHealthChecker::getSchemeType("az://container/blob") == "az");
        REQUIRE(VFSHealthChecker::getSchemeType("azure://container/blob") == "az");
    }

    SECTION("HTTP paths return 'http'") {
        REQUIRE(VFSHealthChecker::getSchemeType("http://example.com/file") == "http");
    }

    SECTION("HTTPS paths return 'https'") {
        REQUIRE(VFSHealthChecker::getSchemeType("https://example.com/file") == "https");
        REQUIRE(VFSHealthChecker::getSchemeType("HTTPS://example.com/file") == "https");
    }
}

// ============================================================================
// VFSHealthChecker::checkPath Tests
// ============================================================================

TEST_CASE("VFSHealthChecker::checkPath with local files", "[vfs][health]") {
    VFSHealthChecker checker;

    SECTION("Existing file is accessible") {
        TempTestFile temp_file("test content");

        auto status = checker.checkPath("config", temp_file.pathString());

        REQUIRE(status.name == "config");
        REQUIRE(status.path == temp_file.pathString());
        REQUIRE(status.accessible == true);
        REQUIRE(status.scheme == "local");
        REQUIRE(status.error.empty());
        REQUIRE(status.latency_ms >= 0);
    }

    SECTION("Non-existent file is not accessible") {
        auto status = checker.checkPath("config", "/nonexistent/path/file.yaml");

        REQUIRE(status.name == "config");
        REQUIRE(status.accessible == false);
        REQUIRE(status.scheme == "local");
        REQUIRE_FALSE(status.error.empty());
    }

    SECTION("Empty path is not accessible") {
        auto status = checker.checkPath("config", "");

        REQUIRE(status.accessible == false);
        REQUIRE(status.error == "Path is empty");
    }

    SECTION("Existing directory is accessible") {
        TempTestDir temp_dir;
        temp_dir.createFile("test.yaml", "content");

        auto status = checker.checkPath("templates", temp_dir.pathString());

        REQUIRE(status.name == "templates");
        REQUIRE(status.accessible == true);
        REQUIRE(status.scheme == "local");
    }
}

// ============================================================================
// VFSHealthChecker::checkHealth Tests
// ============================================================================

TEST_CASE("VFSHealthChecker::checkHealth", "[vfs][health]") {
    VFSHealthChecker checker;

    SECTION("Both paths accessible returns healthy") {
        TempTestFile temp_config("project-name: test");
        TempTestDir temp_templates;
        temp_templates.createFile("endpoint.yaml", "url-path: /test");

        auto health = checker.checkHealth(temp_config.pathString(), temp_templates.pathString());

        REQUIRE(health.healthy == true);
        REQUIRE(health.backends.size() == 2);
        REQUIRE(health.total_latency_ms >= 0);

        // Find config backend
        auto config_it = std::find_if(health.backends.begin(), health.backends.end(),
                                       [](const auto& b) { return b.name == "config"; });
        REQUIRE(config_it != health.backends.end());
        REQUIRE(config_it->accessible == true);

        // Find templates backend
        auto templates_it = std::find_if(health.backends.begin(), health.backends.end(),
                                          [](const auto& b) { return b.name == "templates"; });
        REQUIRE(templates_it != health.backends.end());
        REQUIRE(templates_it->accessible == true);
    }

    SECTION("One path inaccessible returns unhealthy") {
        TempTestFile temp_config("project-name: test");

        auto health = checker.checkHealth(temp_config.pathString(), "/nonexistent/templates");

        REQUIRE(health.healthy == false);
        REQUIRE(health.backends.size() == 2);

        // Config should be accessible
        auto config_it = std::find_if(health.backends.begin(), health.backends.end(),
                                       [](const auto& b) { return b.name == "config"; });
        REQUIRE(config_it != health.backends.end());
        REQUIRE(config_it->accessible == true);

        // Templates should not be accessible
        auto templates_it = std::find_if(health.backends.begin(), health.backends.end(),
                                          [](const auto& b) { return b.name == "templates"; });
        REQUIRE(templates_it != health.backends.end());
        REQUIRE(templates_it->accessible == false);
    }

    SECTION("Empty paths are skipped") {
        TempTestFile temp_config("project-name: test");

        auto health = checker.checkHealth(temp_config.pathString(), "");

        // Only one backend should be checked
        REQUIRE(health.backends.size() == 1);
        REQUIRE(health.backends[0].name == "config");
        REQUIRE(health.healthy == true);
    }

    SECTION("Both paths empty returns healthy with no backends") {
        auto health = checker.checkHealth("", "");

        REQUIRE(health.backends.empty());
        REQUIRE(health.healthy == true);
    }
}

// ============================================================================
// VFSHealthChecker::verifyStartupHealth Tests
// ============================================================================

TEST_CASE("VFSHealthChecker::verifyStartupHealth", "[vfs][health]") {
    VFSHealthChecker checker;

    SECTION("Returns true when all paths accessible") {
        TempTestFile temp_config("project-name: test");
        TempTestDir temp_templates;
        temp_templates.createFile("endpoint.yaml", "content");

        bool result = checker.verifyStartupHealth(temp_config.pathString(),
                                                   temp_templates.pathString());

        REQUIRE(result == true);
    }

    SECTION("Returns false when any path inaccessible") {
        TempTestFile temp_config("project-name: test");

        bool result = checker.verifyStartupHealth(temp_config.pathString(),
                                                   "/nonexistent/templates");

        REQUIRE(result == false);
    }
}

// ============================================================================
// Remote Path Health Checks (scheme detection only, no actual network)
// ============================================================================

TEST_CASE("VFSHealthChecker remote path scheme detection", "[vfs][health]") {
    VFSHealthChecker checker;

    // Note: These tests run without DatabaseManager initialized, so remote paths
    // will correctly report accessible=false with an appropriate error message.
    // This is the expected behavior - the health checker should gracefully handle
    // missing database infrastructure and report status, not crash.

    SECTION("S3 paths have correct scheme") {
        auto status = checker.checkPath("remote", "s3://bucket/key/file.yaml");
        REQUIRE(status.scheme == "s3");
        REQUIRE_FALSE(status.accessible);
        // Should indicate DB not initialized
        REQUIRE(status.error.find("not initialized") != std::string::npos);
    }

    SECTION("GCS paths have correct scheme") {
        auto status = checker.checkPath("remote", "gs://bucket/path/file.yaml");
        REQUIRE(status.scheme == "gs");
        REQUIRE_FALSE(status.accessible);
        REQUIRE(status.error.find("not initialized") != std::string::npos);
    }

    SECTION("Azure paths have correct scheme") {
        auto status = checker.checkPath("remote", "az://container/blob.yaml");
        REQUIRE(status.scheme == "az");
        REQUIRE_FALSE(status.accessible);
        REQUIRE(status.error.find("not initialized") != std::string::npos);
    }

    SECTION("HTTPS paths have correct scheme") {
        auto status = checker.checkPath("remote", "https://example.com/config.yaml");
        REQUIRE(status.scheme == "https");
        REQUIRE_FALSE(status.accessible);
        REQUIRE(status.error.find("not initialized") != std::string::npos);
    }
}
