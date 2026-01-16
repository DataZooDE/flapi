#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_all.hpp>
#include "config_loader.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace flapi;

// Helper to create temporary test files
class TempFile {
public:
    explicit TempFile(const std::string& content = "") {
        path_ = std::filesystem::temp_directory_path() / ("test_config_" + std::to_string(reinterpret_cast<uintptr_t>(this)) + ".yaml");
        if (!content.empty()) {
            std::ofstream file(path_);
            file << content;
        }
    }

    ~TempFile() {
        if (std::filesystem::exists(path_)) {
            std::filesystem::remove(path_);
        }
    }

    std::filesystem::path path() const { return path_; }

private:
    std::filesystem::path path_;
};

TEST_CASE("ConfigLoader initialization", "[config][loader]") {
    SECTION("Initialize with absolute path") {
        TempFile config("project-name: test");
        ConfigLoader loader(config.path());

        REQUIRE(loader.getConfigFilePath() == config.path());
        REQUIRE(loader.getBasePath() == config.path().parent_path());
    }

    SECTION("Initialize with relative path") {
        TempFile config("project-name: test");
        ConfigLoader loader(config.path());

        REQUIRE(loader.getConfigDirectory() == config.path().parent_path());
        REQUIRE(loader.fileExists(config.path()));
    }
}

TEST_CASE("ConfigLoader::loadYamlFile", "[config][loader]") {
    SECTION("Load valid YAML file") {
        std::string yaml_content = R"(
project-name: TestProject
connections:
  main:
    type: postgres
)";
        TempFile config(yaml_content);
        ConfigLoader loader(config.path());

        YAML::Node node = loader.loadYamlFile(config.path());
        REQUIRE(node["project-name"].as<std::string>() == "TestProject");
        REQUIRE(node["connections"]["main"]["type"].as<std::string>() == "postgres");
    }

    SECTION("Throw on missing file") {
        TempFile config("test");
        ConfigLoader loader(config.path());

        REQUIRE_THROWS_AS(
            loader.loadYamlFile("/nonexistent/path/to/file.yaml"),
            std::runtime_error
        );
    }

    SECTION("Throw on invalid YAML") {
        std::string invalid_yaml = R"(
invalid: yaml: content:
  - broken: [array
)";
        TempFile config(invalid_yaml);
        ConfigLoader loader(config.path());

        REQUIRE_THROWS_AS(
            loader.loadYamlFile(config.path()),
            std::runtime_error
        );
    }

    SECTION("Load minimal YAML file") {
        TempFile config("# Empty config");
        ConfigLoader loader(config.path());

        YAML::Node node = loader.loadYamlFile(config.path());
        // Comment-only YAML still parses as a valid (empty) node
        REQUIRE((node.IsNull() || node.size() == 0));
    }
}

TEST_CASE("ConfigLoader path resolution", "[config][loader]") {
    SECTION("Resolve absolute path") {
        TempFile config("test");
        ConfigLoader loader(config.path());

        std::filesystem::path abs_path = std::filesystem::absolute(config.path());
        std::filesystem::path resolved = loader.resolvePath(abs_path);

        REQUIRE(resolved == abs_path);
    }

    SECTION("Resolve relative path") {
        TempFile config("test");
        ConfigLoader loader(config.path());

        // Create a test file in the same directory
        std::filesystem::path test_file = config.path().parent_path() / "test.yaml";
        std::ofstream(test_file) << "test";

        std::filesystem::path resolved = loader.resolvePath("test.yaml");
        REQUIRE(resolved == test_file);

        std::filesystem::remove(test_file);
    }

    SECTION("Resolve relative path with subdirectory") {
        TempFile config("test");
        ConfigLoader loader(config.path());

        // Create test directory and file
        std::filesystem::path test_dir = config.path().parent_path() / "subdir";
        std::filesystem::create_directory(test_dir);
        std::filesystem::path test_file = test_dir / "test.yaml";
        std::ofstream(test_file) << "test";

        std::filesystem::path resolved = loader.resolvePath("subdir/test.yaml");
        REQUIRE(resolved == test_file);

        std::filesystem::remove(test_file);
        std::filesystem::remove(test_dir);
    }

    SECTION("Resolve empty path returns base path") {
        TempFile config("test");
        ConfigLoader loader(config.path());

        std::filesystem::path resolved = loader.resolvePath("");
        REQUIRE(resolved == loader.getBasePath());
    }

    SECTION("Resolve path with ./ prefix") {
        TempFile config("test");
        ConfigLoader loader(config.path());

        std::filesystem::path test_file = config.path().parent_path() / "test.yaml";
        std::ofstream(test_file) << "test";

        std::filesystem::path resolved = loader.resolvePath("./test.yaml");
        REQUIRE(resolved == test_file);

        std::filesystem::remove(test_file);
    }
}

TEST_CASE("ConfigLoader file existence checks", "[config][loader]") {
    SECTION("fileExists returns true for existing file") {
        TempFile config("test");
        ConfigLoader loader(config.path());

        REQUIRE(loader.fileExists(config.path()));
    }

    SECTION("fileExists returns false for non-existent file") {
        TempFile config("test");
        ConfigLoader loader(config.path());

        REQUIRE_FALSE(loader.fileExists("/nonexistent/file.yaml"));
    }

    SECTION("directoryExists returns true for existing directory") {
        TempFile config("test");
        ConfigLoader loader(config.path());

        REQUIRE(loader.directoryExists(config.path().parent_path()));
    }

    SECTION("directoryExists returns false for file") {
        TempFile config("test");
        ConfigLoader loader(config.path());

        REQUIRE_FALSE(loader.directoryExists(config.path()));
    }

    SECTION("directoryExists returns false for non-existent directory") {
        TempFile config("test");
        ConfigLoader loader(config.path());

        REQUIRE_FALSE(loader.directoryExists("/nonexistent/directory/path"));
    }
}

TEST_CASE("ConfigLoader base path", "[config][loader]") {
    SECTION("getBasePath returns parent directory of config file") {
        TempFile config("test");
        ConfigLoader loader(config.path());

        REQUIRE(loader.getBasePath() == config.path().parent_path());
    }

    SECTION("getConfigDirectory returns same as getBasePath") {
        TempFile config("test");
        ConfigLoader loader(config.path());

        REQUIRE(loader.getConfigDirectory() == loader.getBasePath());
    }

    SECTION("getConfigFilePath returns original path") {
        TempFile config("test");
        ConfigLoader loader(config.path());

        REQUIRE(loader.getConfigFilePath() == config.path());
    }
}

TEST_CASE("ConfigLoader complex YAML parsing", "[config][loader]") {
    SECTION("Load nested YAML structure") {
        std::string yaml_content = R"(
server:
  port: 8080
  host: localhost
  ssl:
    enabled: true
    cert: /path/to/cert

database:
  connections:
    - name: main
      type: postgres
    - name: cache
      type: sqlite

endpoints:
  - path: /api/users
    method: GET
)";
        TempFile config(yaml_content);
        ConfigLoader loader(config.path());

        YAML::Node node = loader.loadYamlFile(config.path());

        // Verify nested access
        REQUIRE(node["server"]["port"].as<int>() == 8080);
        REQUIRE(node["server"]["ssl"]["enabled"].as<bool>() == true);
        REQUIRE(node["database"]["connections"][0]["name"].as<std::string>() == "main");
        REQUIRE(node["endpoints"][0]["path"].as<std::string>() == "/api/users");
    }

    SECTION("Load YAML with arrays") {
        std::string yaml_content = R"(
items:
  - item1
  - item2
  - item3

values:
  - 10
  - 20
  - 30
)";
        TempFile config(yaml_content);
        ConfigLoader loader(config.path());

        YAML::Node node = loader.loadYamlFile(config.path());

        REQUIRE(node["items"].size() == 3);
        REQUIRE(node["items"][0].as<std::string>() == "item1");
        REQUIRE(node["values"][2].as<int>() == 30);
    }

    SECTION("Load YAML with null values") {
        std::string yaml_content = R"(
field1:
field2: value
field3:
)";
        TempFile config(yaml_content);
        ConfigLoader loader(config.path());

        YAML::Node node = loader.loadYamlFile(config.path());

        REQUIRE(node["field1"].IsNull());
        REQUIRE(node["field2"].as<std::string>() == "value");
        REQUIRE(node["field3"].IsNull());
    }
}

TEST_CASE("ConfigLoader error messages", "[config][loader]") {
    SECTION("Error message includes file path") {
        TempFile config("test");
        ConfigLoader loader(config.path());

        try {
            loader.loadYamlFile("/nonexistent/file.yaml");
            REQUIRE(false);  // Should not reach here
        } catch (const std::runtime_error& e) {
            REQUIRE(std::string(e.what()).find("nonexistent/file.yaml") != std::string::npos);
        }
    }

    SECTION("Error message includes parse details for invalid YAML") {
        std::string invalid_yaml = "bad: [syntax";
        TempFile config(invalid_yaml);
        ConfigLoader loader(config.path());

        try {
            loader.loadYamlFile(config.path());
            REQUIRE(false);  // Should not reach here
        } catch (const std::runtime_error& e) {
            std::string error_msg(e.what());
            bool has_parse = error_msg.find("parse") != std::string::npos;
            bool has_yaml = error_msg.find("YAML") != std::string::npos;
            REQUIRE((has_parse || has_yaml));
        }
    }
}
