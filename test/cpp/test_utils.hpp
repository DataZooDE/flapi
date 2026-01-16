#pragma once

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <random>

#include "config_manager.hpp"

namespace flapi {
namespace test {

/**
 * RAII wrapper for a temporary file.
 * Creates file on construction, deletes on destruction.
 */
class TempFile {
public:
    explicit TempFile(const std::string& content,
                      const std::string& filename = "temp_test.yaml")
        : path_(std::filesystem::temp_directory_path() / generateUniqueName(filename)) {
        std::ofstream file(path_);
        file << content;
        file.close();
    }

    ~TempFile() {
        if (std::filesystem::exists(path_)) {
            std::filesystem::remove(path_);
        }
    }

    // Non-copyable, movable
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
    TempFile(TempFile&& other) noexcept : path_(std::move(other.path_)) {
        other.path_.clear();
    }
    TempFile& operator=(TempFile&& other) noexcept {
        if (this != &other) {
            cleanup();
            path_ = std::move(other.path_);
            other.path_.clear();
        }
        return *this;
    }

    std::string path() const { return path_.string(); }
    std::filesystem::path fsPath() const { return path_; }

private:
    std::filesystem::path path_;

    void cleanup() {
        if (!path_.empty() && std::filesystem::exists(path_)) {
            std::filesystem::remove(path_);
        }
    }

    static std::string generateUniqueName(const std::string& base) {
        static std::random_device rd;
        static std::mt19937 gen(rd());
        static std::uniform_int_distribution<> dis(10000, 99999);

        auto stem = std::filesystem::path(base).stem().string();
        auto ext = std::filesystem::path(base).extension().string();
        return stem + "_" + std::to_string(dis(gen)) + ext;
    }
};

/**
 * RAII wrapper for a temporary directory.
 * Creates directory on construction, recursively deletes on destruction.
 */
class TempDirectory {
public:
    explicit TempDirectory(const std::string& prefix = "flapi_test")
        : path_(std::filesystem::temp_directory_path() / generateUniqueName(prefix)) {
        std::filesystem::create_directories(path_);
    }

    ~TempDirectory() {
        cleanup();
    }

    // Non-copyable, movable
    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;
    TempDirectory(TempDirectory&& other) noexcept : path_(std::move(other.path_)) {
        other.path_.clear();
    }
    TempDirectory& operator=(TempDirectory&& other) noexcept {
        if (this != &other) {
            cleanup();
            path_ = std::move(other.path_);
            other.path_.clear();
        }
        return *this;
    }

    std::string path() const { return path_.string(); }
    std::filesystem::path fsPath() const { return path_; }

    // Create a subdirectory
    std::filesystem::path createSubdir(const std::string& name) {
        auto subdir = path_ / name;
        std::filesystem::create_directories(subdir);
        return subdir;
    }

    // Create a file in this directory
    std::filesystem::path writeFile(const std::string& filename, const std::string& content) {
        auto file_path = path_ / filename;
        std::ofstream file(file_path);
        file << content;
        file.close();
        return file_path;
    }

private:
    std::filesystem::path path_;

    void cleanup() {
        if (!path_.empty() && std::filesystem::exists(path_)) {
            std::filesystem::remove_all(path_);
        }
    }

    static std::string generateUniqueName(const std::string& prefix) {
        static std::random_device rd;
        static std::mt19937 gen(rd());
        static std::uniform_int_distribution<> dis(10000, 99999);
        return prefix + "_" + std::to_string(dis(gen));
    }
};

/**
 * Complete test configuration environment.
 * Creates a temp directory with flapi.yaml and sqls subdirectory.
 * Cleans up everything on destruction.
 */
class TempTestConfig {
public:
    explicit TempTestConfig(const std::string& prefix = "flapi_test")
        : dir_(prefix) {
        // Create sqls subdirectory
        sqls_path_ = dir_.createSubdir("sqls");

        // Create default flapi.yaml
        std::string config_content = R"(
project-name: test-project
project-description: Test project
http-port: 8080
template:
  path: ./sqls
connections:
  test:
    properties:
      path: ./data.parquet
)";
        config_path_ = dir_.writeFile("flapi.yaml", config_content);
    }

    // Create with custom config content
    explicit TempTestConfig(const std::string& config_content, const std::string& prefix)
        : dir_(prefix) {
        sqls_path_ = dir_.createSubdir("sqls");
        config_path_ = dir_.writeFile("flapi.yaml", config_content);
    }

    std::string dirPath() const { return dir_.path(); }
    std::string configPath() const { return config_path_.string(); }
    std::string sqlsPath() const { return sqls_path_.string(); }

    // Write an endpoint config file to sqls directory
    std::filesystem::path writeEndpoint(const std::string& filename, const std::string& content) {
        auto file_path = sqls_path_ / filename;
        std::ofstream file(file_path);
        file << content;
        file.close();
        return file_path;
    }

    // Write a SQL template file to sqls directory
    std::filesystem::path writeSqlTemplate(const std::string& filename, const std::string& content) {
        return writeEndpoint(filename, content);
    }

    // Create and load a ConfigManager
    std::shared_ptr<ConfigManager> createConfigManager() {
        auto mgr = std::make_shared<ConfigManager>(config_path_.string());
        mgr->loadConfig();
        return mgr;
    }

private:
    TempDirectory dir_;
    std::filesystem::path config_path_;
    std::filesystem::path sqls_path_;
};

// Convenience function for creating temp YAML files (backward compatibility)
inline std::string createTempYamlFile(const std::string& content,
                                       const std::string& filename = "temp_config.yaml") {
    // Note: Caller is responsible for cleanup. Prefer TempFile RAII class.
    std::filesystem::path temp_file = std::filesystem::temp_directory_path() / filename;
    std::ofstream file(temp_file);
    file << content;
    file.close();
    return temp_file.string();
}

// Convenience function for creating minimal flapi config
inline std::string createMinimalFlapiConfig(const std::string& template_path = "/tmp") {
    return R"(
project-name: test_project
project-description: Test Description
http-port: 8080
template:
  path: )" + template_path + R"(
connections:
  test_db:
    init: "SELECT 1"
    properties:
      database: ":memory:"
)";
}

} // namespace test
} // namespace flapi
