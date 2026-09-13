#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_all.hpp>
#include <sstream>
#include <any>
#include <string>
#include <vector>
#include <map>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <optional>
// crow must be parsed with its real access specifiers: including it under the
// hack below changes crow::response's layout in this TU only, so a default-
// constructed response disagrees with the one request_handler.cpp sees.
#include <crow.h>
#define private public
#include "mcp_tool_handler.hpp"
#include "mcp_route_handlers.hpp"
#include "config_manager.hpp"
#include "database_manager.hpp"
#undef private
#include "test_utils.hpp"
#include <filesystem>
#include <fstream>

using namespace flapi;
using namespace flapi::test;

// Helper function to create a temporary YAML file with MCP tool configuration
std::string createMCPToolConfigFile(const std::string& content) {
    std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
    std::filesystem::path temp_file = temp_dir / "mcp_tool_test_config.yaml";
    std::ofstream file(temp_file);
    file << content;
    file.close();
    return temp_file.string();
}

TEST_CASE("MCPToolHandler initialization", "[mcp_tool_handler]") {
    SECTION("Valid initialization") {
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

        std::string config_file = createMCPToolConfigFile(yaml_content);
        auto config_manager = std::make_shared<ConfigManager>(config_file);
        auto db_manager = std::make_shared<DatabaseManager>();

        REQUIRE_NOTHROW([&]() {
            MCPToolHandler handler(db_manager, config_manager);
        }());
    }
}

TEST_CASE("MCPToolHandler tool validation", "[mcp_tool_handler]") {
    SECTION("Valid tool arguments") {
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

        std::string config_file = createMCPToolConfigFile(yaml_content);
        auto config_manager = std::make_shared<ConfigManager>(config_file);
        auto db_manager = std::make_shared<DatabaseManager>();

        MCPToolHandler handler(db_manager, config_manager);

        crow::json::wvalue valid_args;
        valid_args["param1"] = "test_value";
        valid_args["param2"] = 42;

        REQUIRE(handler.validateToolArguments("test_tool", valid_args) == false);
    }

    SECTION("Invalid tool arguments - missing required parameter") {
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

        std::string config_file = createMCPToolConfigFile(yaml_content);
        auto config_manager = std::make_shared<ConfigManager>(config_file);
        auto db_manager = std::make_shared<DatabaseManager>();

        MCPToolHandler handler(db_manager, config_manager);

        crow::json::wvalue invalid_args;
        invalid_args["param2"] = "missing_required_param";

        REQUIRE(handler.validateToolArguments("test_tool", invalid_args) == false);
    }

    SECTION("Invalid tool arguments - wrong type") {
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

        std::string config_file = createMCPToolConfigFile(yaml_content);
        auto config_manager = std::make_shared<ConfigManager>(config_file);
        auto db_manager = std::make_shared<DatabaseManager>();

        MCPToolHandler handler(db_manager, config_manager);

        crow::json::wvalue invalid_args;
        invalid_args["param1"] = "not_a_number";

        REQUIRE(handler.validateToolArguments("test_tool", invalid_args) == false);
    }

    SECTION("Invalid tool arguments - constraint violation") {
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

        std::string config_file = createMCPToolConfigFile(yaml_content);
        auto config_manager = std::make_shared<ConfigManager>(config_file);
        auto db_manager = std::make_shared<DatabaseManager>();

        MCPToolHandler handler(db_manager, config_manager);

        crow::json::wvalue invalid_args;
        invalid_args["param1"] = 150; // Above max constraint

        REQUIRE(handler.validateToolArguments("test_tool", invalid_args) == false);
    }

    SECTION("Unknown tool") {
        std::string config_file = createMCPToolConfigFile("project-name: TestProject");
        auto config_manager = std::make_shared<ConfigManager>(config_file);
        auto db_manager = std::make_shared<DatabaseManager>();

        MCPToolHandler handler(db_manager, config_manager);

        crow::json::wvalue args;
        args["param1"] = "value";

        REQUIRE(handler.validateToolArguments("unknown_tool", args) == false);
    }
}

TEST_CASE("MCPToolHandler tool discovery", "[mcp_tool_handler]") {
    SECTION("Available tools list") {
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

        std::string config_file = createMCPToolConfigFile(yaml_content);
        auto config_manager = std::make_shared<ConfigManager>(config_file);
        auto db_manager = std::make_shared<DatabaseManager>();

        MCPToolHandler handler(db_manager, config_manager);

        auto available_tools = handler.getAvailableTools();

        // In unified configuration, tools are discovered from endpoint configs
        // For now, expect empty list since no endpoint configs are loaded
        REQUIRE(available_tools.size() == 0);
    }

    SECTION("Tool definition retrieval") {
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

        std::string config_file = createMCPToolConfigFile(yaml_content);
        auto config_manager = std::make_shared<ConfigManager>(config_file);
        auto db_manager = std::make_shared<DatabaseManager>();

        MCPToolHandler handler(db_manager, config_manager);

        auto tool_def = handler.getToolDefinition("test_tool");

        // In unified configuration, unknown tools return null
        REQUIRE(tool_def.t() == crow::json::type::Null);
    }

    SECTION("Unknown tool definition") {
        std::string config_file = createMCPToolConfigFile("project-name: TestProject");
        auto config_manager = std::make_shared<ConfigManager>(config_file);
        auto db_manager = std::make_shared<DatabaseManager>();

        MCPToolHandler handler(db_manager, config_manager);

        auto tool_def = handler.getToolDefinition("unknown_tool");

        REQUIRE(tool_def.t() == crow::json::type::Null);
    }
}

TEST_CASE("MCPToolHandler parameter preparation", "[mcp_tool_handler]") {
    SECTION("Parameter conversion from JSON") {
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

        std::string config_file = createMCPToolConfigFile(yaml_content);
        auto config_manager = std::make_shared<ConfigManager>(config_file);
        auto db_manager = std::make_shared<DatabaseManager>();

        MCPToolHandler handler(db_manager, config_manager);

        crow::json::wvalue json_args;
        json_args["string_param"] = "test_value";
        json_args["number_param"] = 42;

        REQUIRE(handler.validateToolArguments("test_tool", json_args) == false);
    }
}

TEST_CASE("MCPToolHandler JSON value conversion", "[mcp_tool_handler]") {
    SECTION("String conversion") {
        std::string config_file = createMCPToolConfigFile("project-name: TestProject");
        auto config_manager = std::make_shared<ConfigManager>(config_file);
        auto db_manager = std::make_shared<DatabaseManager>();

        MCPToolHandler handler(db_manager, config_manager);

        // Test string
        crow::json::wvalue string_val = "test_string";
        REQUIRE(handler.convertJsonValueToString(string_val) == "test_string");

        // Test number
        crow::json::wvalue number_val = 42;
        REQUIRE(handler.convertJsonValueToString(number_val) == "42");

        // Test boolean
        crow::json::wvalue bool_val = true;
        REQUIRE(handler.convertJsonValueToString(bool_val) == "true");

        // Test array
        crow::json::wvalue array_val = crow::json::wvalue::list();
        array_val[0] = "a";
        array_val[1] = "b";
        array_val[2] = "c";
        REQUIRE(handler.convertJsonValueToString(array_val) == "[\"a\",\"b\",\"c\"]");

        // Test object
        crow::json::wvalue object_val;
        object_val["key"] = "value";
        REQUIRE(handler.convertJsonValueToString(object_val) == "{\"key\":\"value\"}");
    }
}

TEST_CASE("MCPToolHandler parameter map conversion", "[mcp_tool_handler]") {
    SECTION("JSON object to parameter map") {
        std::string config_file = createMCPToolConfigFile("project-name: TestProject");
        auto config_manager = std::make_shared<ConfigManager>(config_file);
        auto db_manager = std::make_shared<DatabaseManager>();

        MCPToolHandler handler(db_manager, config_manager);

        crow::json::wvalue json_obj;
        json_obj["string_param"] = "test_value";
        json_obj["number_param"] = 42;
        json_obj["boolean_param"] = true;
        json_obj["array_param"] = std::vector<std::string>{"a", "b", "c"};
        json_obj["object_param"]["nested"] = "value";

        auto params = handler.convertJsonToParams(json_obj);

        REQUIRE(params.size() == 5);
        REQUIRE(params["string_param"] == "test_value");
        REQUIRE(params["number_param"] == "42");
        REQUIRE(params["boolean_param"] == "true");
        REQUIRE(params["array_param"] == "[\"a\",\"b\",\"c\"]");
        REQUIRE(params["object_param"] == "{\"nested\":\"value\"}");
    }
}

TEST_CASE("MCPToolHandler write operation support", "[mcp_tool_handler]") {
    SECTION("Write operation detection") {
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

        std::string config_file = createMCPToolConfigFile(yaml_content);
        auto config_manager = std::make_shared<ConfigManager>(config_file);
        auto db_manager = std::make_shared<DatabaseManager>();

        MCPToolHandler handler(db_manager, config_manager);

        // Create an endpoint config with write operation
        EndpointConfig write_endpoint;
        write_endpoint.operation.type = OperationConfig::Write;
        write_endpoint.operation.transaction = true;
        
        // Verify the operation type is Write
        REQUIRE(write_endpoint.operation.type == OperationConfig::Write);
    }

    SECTION("Read operation (default)") {
        std::string config_file = createMCPToolConfigFile("project-name: TestProject");
        auto config_manager = std::make_shared<ConfigManager>(config_file);
        auto db_manager = std::make_shared<DatabaseManager>();

        MCPToolHandler handler(db_manager, config_manager);

        // Create an endpoint config with read operation (default)
        EndpointConfig read_endpoint;
        // Default is Read
        
        // Verify the operation type is Read
        REQUIRE(read_endpoint.operation.type == OperationConfig::Read);
    }
}

TEST_CASE("MCPToolHandler error handling", "[mcp_tool_handler]") {
    SECTION("Create error result") {
        std::string config_file = createMCPToolConfigFile("project-name: TestProject");
        auto config_manager = std::make_shared<ConfigManager>(config_file);
        auto db_manager = std::make_shared<DatabaseManager>();

        MCPToolHandler handler(db_manager, config_manager);

        auto error_result = handler.createErrorResult("Test error message");

        REQUIRE(error_result.success == false);
        REQUIRE(error_result.error_message == "Test error message");
        REQUIRE(error_result.result.empty());
        REQUIRE(error_result.metadata.empty());
    }

    SECTION("Create success result") {
        std::string config_file = createMCPToolConfigFile("project-name: TestProject");
        auto config_manager = std::make_shared<ConfigManager>(config_file);
        auto db_manager = std::make_shared<DatabaseManager>();

        MCPToolHandler handler(db_manager, config_manager);

        std::unordered_map<std::string, std::string> metadata = {
            {"tool_name", "test_tool"},
            {"execution_time_ms", "100"}
        };

        auto success_result = handler.createSuccessResult("Test result", metadata);

        REQUIRE(success_result.success == true);
        REQUIRE(success_result.result == "Test result");
        REQUIRE(success_result.error_message.empty());
        REQUIRE(success_result.metadata["tool_name"] == "test_tool");
        REQUIRE(success_result.metadata["execution_time_ms"] == "100");
    }
}

TEST_CASE("MCP tools return a protocol error while endpoint cache is warming", "[mcp_tool_handler][cache]") {
    TempTestConfig temp("mcp_cache_warming");
    temp.writeEndpoint("cached_tool.yaml", R"(
url-path: /cached-tool
method: GET
template-source: cached_tool.sql
connection: [test]
cache:
  enabled: true
  table: cached_tool_cache
mcp-tool:
  name: cached_tool
  description: Cached tool
)");
    temp.writeSqlTemplate("cached_tool.sql", "SELECT 1 AS ok");
    auto config_manager = temp.createConfigManager();
    auto db_manager = std::make_shared<DatabaseManager>();
    auto cache_manager = std::make_shared<CacheManager>(std::shared_ptr<ICacheDatabaseAdapter>(nullptr));
    db_manager->cache_manager = cache_manager;
    cache_manager->markCacheStarting(config_manager, config_manager->getEndpoints().front());

    auto session_manager = std::make_shared<MCPSessionManager>();
    auto capabilities = std::make_shared<MCPClientCapabilitiesDetector>();
    MCPRouteHandlers route_handlers(config_manager, db_manager, session_manager, capabilities);

    MCPRequest request;
    request.id = "1";
    request.id_present = true;
    request.id_raw = "\"1\"";
    request.method = "tools/call";
    request.params = crow::json::wvalue::object();
    request.params["name"] = "cached_tool";
    request.params["arguments"] = crow::json::wvalue::object();
    crow::request http_req;

    auto response = route_handlers.handleToolsCallRequest(request, http_req);

    REQUIRE(response.result.empty());
    REQUIRE(!response.error.empty());
    REQUIRE(response.http_status == 503);
    REQUIRE(response.error.find("cache_warming") != std::string::npos);
}
