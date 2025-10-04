#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include "config_manager.hpp"

using namespace flapi;

namespace {
    // Helper to create a temporary test file
    class TempFile {
    public:
        TempFile(const std::string& content) {
            path = std::filesystem::temp_directory_path() / ("test_" + std::to_string(rand()) + ".yaml");
            std::ofstream file(path);
            file << content;
            file.close();
        }
        
        ~TempFile() {
            if (std::filesystem::exists(path)) {
                std::filesystem::remove(path);
            }
        }
        
        std::filesystem::path path;
    };
    
    // Helper to create a minimal valid flapi config
    std::filesystem::path createTestFlapiConfig() {
        auto temp_path = std::filesystem::temp_directory_path() / ("flapi_test_" + std::to_string(rand()) + ".yaml");
        auto template_dir = std::filesystem::temp_directory_path() / ("test_templates_" + std::to_string(rand()));
        
        // Create template directory
        std::filesystem::create_directories(template_dir);
        
        std::ofstream file(temp_path);
        file << "project_name: test\n";  // Use underscore not hyphen
        file << "project_description: Test project\n";  // Required field
        file << "http_port: 8080\n";      // Use underscore not hyphen
        file << "connections:\n";
        file << "  default:\n";
        file << "    init: \"SELECT 1\"\n";
        file << "template:\n";
        file << "  path: " << template_dir.string() << "\n";
        file.close();
        return temp_path;
    }
}

TEST_CASE("YAML Validation - Valid REST Endpoint", "[config_manager][yaml][validation]") {
    auto config_file = createTestFlapiConfig();
    ConfigManager manager(config_file);
    manager.loadConfig(); // Must load config for connections to be available
    
    std::string valid_yaml = R"(
url-path: /api/users
method: GET
template-source: users.sql
connection:
  - default
)";
    
    auto result = manager.validateEndpointConfigFromYaml(valid_yaml);
    
    REQUIRE(result.valid == true);
    REQUIRE(result.errors.empty());
    
    // Clean up
    std::filesystem::remove(config_file);
}

TEST_CASE("YAML Validation - Valid MCP Tool", "[config_manager][yaml][validation]") {
    auto config_file = createTestFlapiConfig();
    ConfigManager manager(config_file);
    manager.loadConfig();
    
    std::string valid_yaml = R"(
mcp-tool:
  name: list_users
  description: Lists all users
  result-mime-type: application/json
template-source: users.sql
connection:
  - default
)";
    
    auto result = manager.validateEndpointConfigFromYaml(valid_yaml);
    
    REQUIRE(result.valid == true);
    REQUIRE(result.errors.empty());
    
    std::filesystem::remove(config_file);
}

TEST_CASE("YAML Validation - Invalid YAML Syntax", "[config_manager][yaml][validation]") {
    auto config_file = createTestFlapiConfig();
    ConfigManager manager(config_file);
    manager.loadConfig();
    
    std::string invalid_yaml = R"(
url-path: /api/users
method: GET
template-source: users.sql
connection:
  - default
  invalid: [unclosed
)";
    
    auto result = manager.validateEndpointConfigFromYaml(invalid_yaml);
    
    REQUIRE(result.valid == false);
    REQUIRE_FALSE(result.errors.empty());
    REQUIRE(result.errors[0].find("YAML parsing error") != std::string::npos);
    
    std::filesystem::remove(config_file);
}

TEST_CASE("YAML Validation - Missing Required Fields", "[config_manager][yaml][validation]") {
    auto config_file = createTestFlapiConfig();
    ConfigManager manager(config_file);
    manager.loadConfig();
    
    std::string missing_fields = R"(
url-path: /api/users
# Missing template-source
connection:
  - default
)";
    
    auto result = manager.validateEndpointConfigFromYaml(missing_fields);
    
    REQUIRE(result.valid == false);
    REQUIRE_FALSE(result.errors.empty());
    
    std::filesystem::remove(config_file);
}

TEST_CASE("YAML Validation - Invalid Connection", "[config_manager][yaml][validation]") {
    auto config_file = createTestFlapiConfig();
    ConfigManager manager(config_file);
    manager.loadConfig();
    
    std::string invalid_connection = R"(
url-path: /api/users
method: GET
template-source: users.sql
connection:
  - nonexistent_connection
)";
    
    auto result = manager.validateEndpointConfigFromYaml(invalid_connection);
    
    REQUIRE(result.valid == false);
    REQUIRE_FALSE(result.errors.empty());
    bool found_error = false;
    for (const auto& error : result.errors) {
        if (error.find("Connection") != std::string::npos && error.find("not found") != std::string::npos) {
            found_error = true;
            break;
        }
    }
    REQUIRE(found_error);
    
    std::filesystem::remove(config_file);
}

TEST_CASE("YAML Validation - Invalid URL Path", "[config_manager][yaml][validation]") {
    auto config_file = createTestFlapiConfig();
    ConfigManager manager(config_file);
    manager.loadConfig();
    
    std::string invalid_path = R"(
url-path: api/users
method: GET
template-source: users.sql
connection:
  - default
)";
    
    auto result = manager.validateEndpointConfigFromYaml(invalid_path);
    
    REQUIRE(result.valid == false);
    REQUIRE_FALSE(result.errors.empty());
    bool found_error = false;
    for (const auto& error : result.errors) {
        if (error.find("url-path") != std::string::npos || error.find("start with '/'") != std::string::npos) {
            found_error = true;
            break;
        }
    }
    REQUIRE(found_error);
    
    std::filesystem::remove(config_file);
}

TEST_CASE("YAML Validation - Warnings for Missing Template File", "[config_manager][yaml][validation]") {
    auto config_file = createTestFlapiConfig();
    ConfigManager manager(config_file);
    manager.loadConfig();
    
    std::string yaml_with_missing_template = R"(
url-path: /api/users
method: GET
template-source: nonexistent.sql
connection:
  - default
)";
    
    auto result = manager.validateEndpointConfigFromYaml(yaml_with_missing_template);
    
    // Should be valid but with warnings
    REQUIRE(result.valid == true);
    REQUIRE_FALSE(result.warnings.empty());
    bool found_warning = false;
    for (const auto& warning : result.warnings) {
        if (warning.find("Template file") != std::string::npos && warning.find("does not exist") != std::string::npos) {
            found_warning = true;
            break;
        }
    }
    REQUIRE(found_warning);
    
    std::filesystem::remove(config_file);
}

TEST_CASE("YAML Validation - File Validation", "[config_manager][yaml][validation]") {
    auto config_file = createTestFlapiConfig();
    ConfigManager manager(config_file);
    manager.loadConfig();
    
    // Create a temporary YAML file
    std::string valid_yaml = R"(
url-path: /api/users
method: GET
template-source: users.sql
connection:
  - default
)";
    
    TempFile temp_yaml(valid_yaml);
    
    auto result = manager.validateEndpointConfigFile(temp_yaml.path);
    
    REQUIRE(result.valid == true);
    REQUIRE(result.errors.empty());
    
    std::filesystem::remove(config_file);
}

TEST_CASE("YAML Validation - File Not Found", "[config_manager][yaml][validation]") {
    auto config_file = createTestFlapiConfig();
    ConfigManager manager(config_file);
    manager.loadConfig();
    
    auto result = manager.validateEndpointConfigFile("/nonexistent/file.yaml");
    
    REQUIRE(result.valid == false);
    REQUIRE_FALSE(result.errors.empty());
    REQUIRE(result.errors[0].find("does not exist") != std::string::npos);
    
    std::filesystem::remove(config_file);
}

TEST_CASE("YAML Validation - No Endpoint Type Defined", "[config_manager][yaml][validation]") {
    auto config_file = createTestFlapiConfig();
    ConfigManager manager(config_file);
    manager.loadConfig();
    
    std::string no_endpoint_type = R"(
# No url-path, no mcp-tool, no mcp-resource, no mcp-prompt
template-source: users.sql
connection:
  - default
)";
    
    auto result = manager.validateEndpointConfigFromYaml(no_endpoint_type);
    
    REQUIRE(result.valid == false);
    REQUIRE_FALSE(result.errors.empty());
    
    std::filesystem::remove(config_file);
}

TEST_CASE("YAML Serialization Preserves Structure", "[config_manager][yaml][serialization]") {
    auto config_file = createTestFlapiConfig();
    ConfigManager manager(config_file);
    manager.loadConfig();
    
    // Create an endpoint config
    EndpointConfig config;
    config.urlPath = "/api/test";
    config.method = "POST";
    config.templateSource = "test.sql";
    config.connection = {"default"};
    config.cache.enabled = true;
    config.cache.table = "test_cache";
    
    // Serialize to YAML
    std::string yaml = manager.serializeEndpointConfigToYaml(config);
    
    // Should contain expected fields
    REQUIRE(yaml.find("url-path: /api/test") != std::string::npos);
    REQUIRE(yaml.find("method: POST") != std::string::npos);
    REQUIRE(yaml.find("template-source: test.sql") != std::string::npos);
    REQUIRE(yaml.find("cache:") != std::string::npos);
    REQUIRE(yaml.find("enabled: true") != std::string::npos);
    
    // Should be valid when deserialized
    auto result = manager.validateEndpointConfigFromYaml(yaml);
    REQUIRE(result.valid == true);
    
    std::filesystem::remove(config_file);
}

