#include <catch2/catch_test_macros.hpp>
#include "../../src/include/endpoint_config_parser.hpp"
#include "../../src/include/config_manager.hpp"
#include <fstream>
#include <filesystem>

using namespace flapi;
namespace fs = std::filesystem;

namespace {
// Helper to create a temporary YAML file
std::string createTempYamlFile(const std::string& content, const std::string& filename = "test.yaml") {
    fs::path temp_file = fs::temp_directory_path() / filename;
    std::ofstream file(temp_file);
    file << content;
    file.close();
    return temp_file.string();
}

// Helper to create a minimal flapi config for ConfigManager
std::string createMinimalFlapiConfig() {
    std::string content = R"(
project_name: test_project
project_description: Test Description
http_port: 8080
template:
  path: /tmp
connections:
  test_db:
    init: "SELECT 1"
    properties:
      database: ":memory:"
)";
    return createTempYamlFile(content, "flapi_config.yaml");
}
} // anonymous namespace

TEST_CASE("EndpointConfigParser: Parse REST endpoint", "[endpoint_parser]") {
    std::string yaml_content = R"(
url-path: /test
method: GET
template-source: test.sql
connection:
  - test_db
)";
    
    std::string yaml_file = createTempYamlFile(yaml_content, "endpoint_rest_test.yaml");
    std::string config_file = createMinimalFlapiConfig();
    
    ConfigManager manager{fs::path(config_file)};
    // Don't call loadConfig() - we want to test the parser directly
    
    EndpointConfigParser parser(manager.getYamlParser(), &manager);
    auto result = parser.parseFromFile(yaml_file);
    
    REQUIRE(result.success == true);
    REQUIRE(result.config.urlPath == "/test");
    REQUIRE(result.config.method == "GET");
    REQUIRE(result.config.isRESTEndpoint() == true);
    
    fs::remove(yaml_file);
    fs::remove(config_file);
}

TEST_CASE("EndpointConfigParser: Parse MCP Tool", "[endpoint_parser]") {
    std::string yaml_content = R"(
mcp-tool:
  name: test_tool
  description: Test tool description
  result_mime_type: application/json
template-source: test.sql
connection:
  - test_db
)";
    
    std::string yaml_file = createTempYamlFile(yaml_content);
    std::string config_file = createMinimalFlapiConfig();
    
    ConfigManager manager{fs::path(config_file)};
    // Don't call loadConfig() - we want to test the parser directly
    
    EndpointConfigParser parser(manager.getYamlParser(), &manager);
    auto result = parser.parseFromFile(yaml_file);
    
    REQUIRE(result.success == true);
    REQUIRE(result.config.isMCPTool() == true);
    REQUIRE(result.config.mcp_tool->name == "test_tool");
    REQUIRE(result.config.mcp_tool->description == "Test tool description");
    
    fs::remove(yaml_file);
    fs::remove(config_file);
}

TEST_CASE("EndpointConfigParser: Parse MCP Prompt", "[endpoint_parser]") {
    std::string yaml_content = R"(
mcp-prompt:
  name: test_prompt
  description: Test prompt description
  template: |
    You are a helpful assistant.
    
    {{#customer_id}}
    Customer ID: {{customer_id}}
    {{/customer_id}}
    
    Please provide analysis.
  
  arguments:
    - customer_id
    - segment
)";
    
    std::string yaml_file = createTempYamlFile(yaml_content);
    std::string config_file = createMinimalFlapiConfig();
    
    ConfigManager manager{fs::path(config_file)};
    // Don't call loadConfig() - we want to test the parser directly
    
    EndpointConfigParser parser(manager.getYamlParser(), &manager);
    auto result = parser.parseFromFile(yaml_file);
    
    INFO("Parse result: " << (result.success ? "success" : "failed"));
    INFO("Error message: " << result.error_message);
    
    REQUIRE(result.success == true);
    REQUIRE(result.config.isMCPPrompt() == true);
    REQUIRE(result.config.mcp_prompt->name == "test_prompt");
    REQUIRE(result.config.mcp_prompt->description == "Test prompt description");
    REQUIRE(result.config.mcp_prompt->arguments.size() == 2);
    REQUIRE(result.config.mcp_prompt->arguments[0] == "customer_id");
    REQUIRE(result.config.mcp_prompt->arguments[1] == "segment");
    REQUIRE_FALSE(result.config.mcp_prompt->template_content.empty());
    
    fs::remove(yaml_file);
    fs::remove(config_file);
}

TEST_CASE("EndpointConfigParser: Parse MCP Prompt without arguments", "[endpoint_parser]") {
    std::string yaml_content = R"(
mcp-prompt:
  name: simple_prompt
  description: Simple prompt
  template: |
    This is a simple prompt template.
)";
    
    std::string yaml_file = createTempYamlFile(yaml_content);
    std::string config_file = createMinimalFlapiConfig();
    
    ConfigManager manager{fs::path(config_file)};
    // Don't call loadConfig() - we want to test the parser directly
    
    EndpointConfigParser parser(manager.getYamlParser(), &manager);
    auto result = parser.parseFromFile(yaml_file);
    
    INFO("Parse result: " << (result.success ? "success" : "failed"));
    INFO("Error message: " << result.error_message);
    
    REQUIRE(result.success == true);
    REQUIRE(result.config.isMCPPrompt() == true);
    REQUIRE(result.config.mcp_prompt->arguments.empty());
    
    fs::remove(yaml_file);
    fs::remove(config_file);
}

TEST_CASE("EndpointConfigParser: Parse MCP Prompt missing template", "[endpoint_parser]") {
    std::string yaml_content = R"(
mcp-prompt:
  name: bad_prompt
  description: Prompt without template
  arguments:
    - test
)";
    
    std::string yaml_file = createTempYamlFile(yaml_content);
    std::string config_file = createMinimalFlapiConfig();
    
    ConfigManager manager{fs::path(config_file)};
    // Don't call loadConfig() - we want to test the parser directly
    
    EndpointConfigParser parser(manager.getYamlParser(), &manager);
    auto result = parser.parseFromFile(yaml_file);
    
    REQUIRE(result.success == false);
    REQUIRE_FALSE(result.error_message.empty());
    REQUIRE(result.error_message.find("template") != std::string::npos);
    
    fs::remove(yaml_file);
    fs::remove(config_file);
}

TEST_CASE("EndpointConfigParser: Invalid YAML", "[endpoint_parser]") {
    std::string yaml_content = R"(
this is: not
  - valid: yaml
    because: [of, bad, structure
)";
    
    std::string yaml_file = createTempYamlFile(yaml_content);
    std::string config_file = createMinimalFlapiConfig();
    
    ConfigManager manager{fs::path(config_file)};
    // Don't call loadConfig() - we want to test the parser directly
    
    EndpointConfigParser parser(manager.getYamlParser(), &manager);
    auto result = parser.parseFromFile(yaml_file);
    
    REQUIRE(result.success == false);
    REQUIRE_FALSE(result.error_message.empty());
    
    fs::remove(yaml_file);
    fs::remove(config_file);
}

