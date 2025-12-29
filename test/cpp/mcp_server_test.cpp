#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_all.hpp>
#include "mcp_server.hpp"
#include "config_manager.hpp"
#include "database_manager.hpp"
#include <filesystem>
#include <fstream>
#include <thread>
#include <future>

using namespace flapi;

// Helper function to create a temporary YAML file with MCP configuration
std::string createMCPConfigFile(const std::string& content) {
    std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
    std::filesystem::path temp_file = temp_dir / "mcp_test_config.yaml";
    std::ofstream file(temp_file);
    file << content;
    file.close();
    return temp_file.string();
}

// Helper function to create a mock MCP tool configuration
std::string createMCPToolConfig() {
    return R"(
tool_name: test_tool
description: A test tool for unit testing
template_source: test_tool.sql
connection:
  - test_connection

parameters:
  - name: param1
    description: First parameter
    type: string
    required: true
  - name: param2
    description: Second parameter
    type: number
    required: false
    default_value: "42"
    constraints:
      min: "0"
      max: "100"

rate_limit:
  enabled: true
  max: 10
  interval: 60

auth:
  enabled: false
)";
}

TEST_CASE("MCPServer initialization", "[mcp_server]") {
    SECTION("Valid unified MCP server configuration") {
        std::string yaml_content = R"(
project-name: TestProject
template:
  path: ./test_templates
connections:
  test_connection:
    init: "SELECT 1;"
    properties:
      db_file: ":memory:"

# MCP is now automatically enabled - no separate configuration needed!
# Configuration files can define MCP tools and resources alongside REST endpoints
)";

        std::string config_file = createMCPConfigFile(yaml_content);
        auto config_manager = std::make_shared<ConfigManager>(config_file);

        // This would normally require a database manager, but for testing initialization
        // we can create a minimal test
        auto db_manager = std::make_shared<DatabaseManager>();

        REQUIRE_NOTHROW([&]() {
            MCPServer server(config_manager, db_manager);
        }());

        // Verify server is initialized with default values (no separate MCP config)
        // Server info should be set to default values
        REQUIRE(server.getServerInfo().name == "flapi-mcp-server");
        REQUIRE(server.getServerInfo().version == "0.3.0");
        REQUIRE(server.getServerInfo().protocol_version == "2024-11-05");
    }
}

TEST_CASE("MCPServer JSON-RPC protocol handling", "[mcp_server]") {
    SECTION("Initialize request handling") {
        std::string yaml_content = R"(
project-name: TestProject
template:
  path: ./test_templates
connections:
  test_connection:
    init: "SELECT 1;"
    properties:
      db_file: ":memory:"
)";

        std::string config_file = createMCPConfigFile(yaml_content);
        auto config_manager = std::make_shared<ConfigManager>(config_file);
        auto db_manager = std::make_shared<DatabaseManager>();

        MCPServer server(config_manager, db_manager);

        // Test initialize request
        crow::json::wvalue init_request;
        init_request["jsonrpc"] = "2.0";
        init_request["id"] = "123";
        init_request["method"] = "initialize";
        init_request["params"]["protocolVersion"] = "2024-11-05";
        init_request["params"]["capabilities"]["tools"] = crow::json::wvalue();
        init_request["params"]["clientInfo"]["name"] = "test-client";
        init_request["params"]["clientInfo"]["version"] = "1.0.0";

        // The actual protocol handling would be tested via HTTP requests
        // This is a unit test for the core logic
        MCPRequest mcp_req;
        mcp_req.id = "123";
        mcp_req.method = "initialize";
        mcp_req.params = init_request["params"];

        auto response = server.handleMessage(mcp_req);

        REQUIRE(response.id == "123");
        REQUIRE(response.error.is_null());
        REQUIRE(!response.result.is_null());
        REQUIRE(response.result["protocolVersion"] == "2024-11-05");
        REQUIRE(response.result["serverInfo"]["name"] == "flapi-mcp-server");
    }

    SECTION("Tools list request handling") {
        std::string yaml_content = R"(
project-name: TestProject
template:
  path: ./test_templates
mcp:
  enabled: true
  tools:
    - )" + createMCPToolConfig() + R"(
)";

        std::string config_file = createMCPConfigFile(yaml_content);
        auto config_manager = std::make_shared<ConfigManager>(config_file);
        auto db_manager = std::make_shared<DatabaseManager>();

        MCPServer server(config_manager, db_manager);

        MCPRequest mcp_req;
        mcp_req.id = "456";
        mcp_req.method = "tools/list";

        auto response = server.handleMessage(mcp_req);

        REQUIRE(response.id == "456");
        REQUIRE(response.error.is_null());
        REQUIRE(!response.result.is_null());
        REQUIRE(response.result.has("tools"));
    }
}

TEST_CASE("MCPServer tool execution", "[mcp_server]") {
    SECTION("Valid tool call") {
        std::string yaml_content = R"(
project-name: TestProject
template:
  path: ./test_templates
connections:
  test_connection:
    init: "SELECT 1;"
    properties:
      db_file: ":memory:"
mcp:
  enabled: true
  tools:
    - )" + createMCPToolConfig() + R"(
)";

        std::string config_file = createMCPConfigFile(yaml_content);
        auto config_manager = std::make_shared<ConfigManager>(config_file);

        // Initialize database with test connection
        DatabaseManager::getInstance()->initializeDBManagerFromConfig(config_manager);

        auto db_manager = DatabaseManager::getInstance();

        MCPServer server(config_manager, db_manager);

        MCPRequest mcp_req;
        mcp_req.id = "789";
        mcp_req.method = "tools/call";
        mcp_req.params["name"] = "test_tool";
        mcp_req.params["arguments"]["param1"] = "test_value";
        mcp_req.params["arguments"]["param2"] = 42;

        auto response = server.handleMessage(mcp_req);

        REQUIRE(response.id == "789");
        // The actual result depends on the SQL template and database setup
        // This test verifies the protocol handling structure
    }
}

TEST_CASE("MCPServer error handling", "[mcp_server]") {
    SECTION("Method not found") {
        std::string config_file = createMCPConfigFile("project-name: TestProject");
        auto config_manager = std::make_shared<ConfigManager>(config_file);
        auto db_manager = std::make_shared<DatabaseManager>();

        MCPServer server(config_manager, db_manager);

        MCPRequest mcp_req;
        mcp_req.id = "999";
        mcp_req.method = "unknown_method";

        auto response = server.handleMessage(mcp_req);

        REQUIRE(response.id == "999");
        REQUIRE(!response.error.is_null());
        REQUIRE(response.error["code"] == -32601);
        REQUIRE(response.error["message"] == "Method not found");
    }

    SECTION("Invalid request parameters") {
        std::string config_file = createMCPConfigFile("project-name: TestProject");
        auto config_manager = std::make_shared<ConfigManager>(config_file);
        auto db_manager = std::make_shared<DatabaseManager>();

        MCPServer server(config_manager, db_manager);

        MCPRequest mcp_req;
        mcp_req.id = "888";
        mcp_req.method = "tools/call";
        mcp_req.params["arguments"]["param1"] = "test"; // Missing required 'name' parameter

        auto response = server.handleMessage(mcp_req);

        REQUIRE(response.id == "888");
        REQUIRE(!response.error.is_null());
        REQUIRE(response.error["code"] == -32602);
    }
}

TEST_CASE("MCPServer tool discovery", "[mcp_server]") {
    SECTION("Tool definitions generation") {
        // Create a unified configuration with MCP tool
        std::string yaml_content = R"(
project-name: TestProject
template:
  path: ./test_templates
connections:
  test_connection:
    init: "SELECT 1;"
    properties:
      db_file: ":memory:"
)";

        std::string config_file = createMCPConfigFile(yaml_content);
        auto config_manager = std::make_shared<ConfigManager>(config_file);
        auto db_manager = std::make_shared<DatabaseManager>();

        MCPServer server(config_manager, db_manager);

        auto tool_definitions = server.getToolDefinitions();

        // In unified configuration, tools are discovered from endpoint configs
        // For now, expect empty list since no endpoint configs are loaded
        REQUIRE(tool_definitions.size() == 0);
    }
}

TEST_CASE("MCP server lifecycle", "[mcp_server]") {
    SECTION("Start and stop server") {
        std::string config_file = createMCPConfigFile("project-name: TestProject");
        auto config_manager = std::make_shared<ConfigManager>(config_file);
        auto db_manager = std::make_shared<DatabaseManager>();

        MCPServer server(config_manager, db_manager);

        // Server should start successfully
        std::future<void> server_future = std::async(std::launch::async, [&server]() {
            server.run(8083);
        });

        // Give it a moment to start
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Server should be stoppable
        server.stop();
        server_future.wait_for(std::chrono::milliseconds(100));

        // Verify server can be restarted
        std::future<void> server_future2 = std::async(std::launch::async, [&server]() {
            server.run(8084);
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        server.stop();
        server_future2.wait_for(std::chrono::milliseconds(100));
    }
}
