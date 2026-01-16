#include <catch2/catch_test_macros.hpp>
#include <crow/json.h>

#include "config_manager.hpp"

namespace flapi {
namespace test {

/**
 * MCP Prompt Configuration Tests
 *
 * Tests for MCP prompt endpoint configuration and the isMCPPrompt() functionality.
 * Full handler logic (prompts/list, prompts/get, template substitution) is tested
 * in integration tests (test/integration/test_mcp_prompts.py).
 */

class MCPPromptConfigTestHelper {
public:
    static EndpointConfig createPromptEndpoint(
        const std::string& name,
        const std::string& description,
        const std::string& template_content,
        const std::vector<std::string>& arguments = {}
    ) {
        EndpointConfig endpoint;
        EndpointConfig::MCPPromptInfo prompt_info;
        prompt_info.name = name;
        prompt_info.description = description;
        prompt_info.template_content = template_content;
        prompt_info.arguments = arguments;
        endpoint.mcp_prompt = prompt_info;
        return endpoint;
    }

    static EndpointConfig createRESTEndpoint(const std::string& path) {
        EndpointConfig endpoint;
        endpoint.urlPath = path;
        endpoint.method = "GET";
        return endpoint;
    }

    static EndpointConfig createMCPToolEndpoint(const std::string& name) {
        EndpointConfig endpoint;
        EndpointConfig::MCPToolInfo tool_info;
        tool_info.name = name;
        tool_info.description = "A test tool";
        endpoint.mcp_tool = tool_info;
        return endpoint;
    }

    static EndpointConfig createMCPResourceEndpoint(const std::string& name) {
        EndpointConfig endpoint;
        EndpointConfig::MCPResourceInfo resource_info;
        resource_info.name = name;
        resource_info.description = "A test resource";
        endpoint.mcp_resource = resource_info;
        return endpoint;
    }
};

TEST_CASE("MCPPromptConfig: isMCPPrompt identifies prompt endpoints", "[mcp][prompts][config]") {
    SECTION("Endpoint with mcp_prompt is identified as prompt") {
        auto endpoint = MCPPromptConfigTestHelper::createPromptEndpoint(
            "test_prompt",
            "Test description",
            "Hello {{name}}"
        );

        REQUIRE(endpoint.isMCPPrompt() == true);
        REQUIRE(endpoint.isMCPTool() == false);
        REQUIRE(endpoint.isMCPResource() == false);
        REQUIRE(endpoint.isRESTEndpoint() == false);
    }

    SECTION("REST endpoint is not identified as prompt") {
        auto endpoint = MCPPromptConfigTestHelper::createRESTEndpoint("/api/test");

        REQUIRE(endpoint.isMCPPrompt() == false);
        REQUIRE(endpoint.isRESTEndpoint() == true);
    }

    SECTION("MCP tool endpoint is not identified as prompt") {
        auto endpoint = MCPPromptConfigTestHelper::createMCPToolEndpoint("test_tool");

        REQUIRE(endpoint.isMCPPrompt() == false);
        REQUIRE(endpoint.isMCPTool() == true);
    }

    SECTION("MCP resource endpoint is not identified as prompt") {
        auto endpoint = MCPPromptConfigTestHelper::createMCPResourceEndpoint("test_resource");

        REQUIRE(endpoint.isMCPPrompt() == false);
        REQUIRE(endpoint.isMCPResource() == true);
    }
}

TEST_CASE("MCPPromptConfig: getType returns correct type", "[mcp][prompts][config]") {
    SECTION("Prompt endpoint returns MCP_Prompt type") {
        auto endpoint = MCPPromptConfigTestHelper::createPromptEndpoint(
            "test_prompt",
            "Test",
            "Content"
        );

        REQUIRE(endpoint.getType() == EndpointConfig::Type::MCP_Prompt);
    }

    SECTION("REST endpoint returns REST type") {
        auto endpoint = MCPPromptConfigTestHelper::createRESTEndpoint("/api/test");

        REQUIRE(endpoint.getType() == EndpointConfig::Type::REST);
    }

    SECTION("Tool endpoint returns MCP_Tool type") {
        auto endpoint = MCPPromptConfigTestHelper::createMCPToolEndpoint("test_tool");

        REQUIRE(endpoint.getType() == EndpointConfig::Type::MCP_Tool);
    }

    SECTION("Resource endpoint returns MCP_Resource type") {
        auto endpoint = MCPPromptConfigTestHelper::createMCPResourceEndpoint("test_resource");

        REQUIRE(endpoint.getType() == EndpointConfig::Type::MCP_Resource);
    }
}

TEST_CASE("MCPPromptConfig: getName returns prompt name", "[mcp][prompts][config]") {
    SECTION("getName returns MCP prompt name") {
        auto endpoint = MCPPromptConfigTestHelper::createPromptEndpoint(
            "my_greeting_prompt",
            "A greeting prompt",
            "Hello!"
        );

        REQUIRE(endpoint.getName() == "my_greeting_prompt");
    }

    SECTION("getName returns tool name for tool endpoint") {
        auto endpoint = MCPPromptConfigTestHelper::createMCPToolEndpoint("my_tool");

        REQUIRE(endpoint.getName() == "my_tool");
    }

    SECTION("getName returns resource name for resource endpoint") {
        auto endpoint = MCPPromptConfigTestHelper::createMCPResourceEndpoint("my_resource");

        REQUIRE(endpoint.getName() == "my_resource");
    }
}

TEST_CASE("MCPPromptConfig: MCPPromptInfo structure", "[mcp][prompts][config]") {
    SECTION("MCPPromptInfo stores all fields correctly") {
        auto endpoint = MCPPromptConfigTestHelper::createPromptEndpoint(
            "test_name",
            "test_description",
            "Hello {{arg1}}, meet {{arg2}}",
            {"arg1", "arg2"}
        );

        REQUIRE(endpoint.mcp_prompt.has_value());
        REQUIRE(endpoint.mcp_prompt->name == "test_name");
        REQUIRE(endpoint.mcp_prompt->description == "test_description");
        REQUIRE(endpoint.mcp_prompt->template_content == "Hello {{arg1}}, meet {{arg2}}");
        REQUIRE(endpoint.mcp_prompt->arguments.size() == 2);
        REQUIRE(endpoint.mcp_prompt->arguments[0] == "arg1");
        REQUIRE(endpoint.mcp_prompt->arguments[1] == "arg2");
    }

    SECTION("MCPPromptInfo with no arguments") {
        auto endpoint = MCPPromptConfigTestHelper::createPromptEndpoint(
            "simple_prompt",
            "A simple prompt",
            "No arguments here"
        );

        REQUIRE(endpoint.mcp_prompt.has_value());
        REQUIRE(endpoint.mcp_prompt->arguments.empty());
    }
}

TEST_CASE("MCPPromptConfig: isMCPEntity includes prompts", "[mcp][prompts][config]") {
    SECTION("Prompt endpoint is an MCP entity") {
        auto endpoint = MCPPromptConfigTestHelper::createPromptEndpoint(
            "test_prompt",
            "Test",
            "Content"
        );

        REQUIRE(endpoint.isMCPEntity() == true);
    }

    SECTION("REST endpoint is not an MCP entity") {
        auto endpoint = MCPPromptConfigTestHelper::createRESTEndpoint("/api/test");

        REQUIRE(endpoint.isMCPEntity() == false);
    }
}

TEST_CASE("MCPPromptConfig: getShortDescription includes prompt info", "[mcp][prompts][config]") {
    SECTION("getShortDescription returns prompt info") {
        auto endpoint = MCPPromptConfigTestHelper::createPromptEndpoint(
            "greeting_prompt",
            "A greeting prompt",
            "Hello"
        );

        std::string desc = endpoint.getShortDescription();
        REQUIRE(desc.find("MCP Prompt") != std::string::npos);
        REQUIRE(desc.find("greeting_prompt") != std::string::npos);
    }
}

TEST_CASE("MCPPromptConfig: validation for empty name", "[mcp][prompts][config]") {
    SECTION("Empty prompt name produces validation error") {
        EndpointConfig endpoint;
        EndpointConfig::MCPPromptInfo prompt_info;
        prompt_info.name = "";  // Empty name
        prompt_info.description = "Test";
        prompt_info.template_content = "Content";
        endpoint.mcp_prompt = prompt_info;

        auto errors = endpoint.validateSelf();
        REQUIRE_FALSE(errors.empty());

        bool found_name_error = false;
        for (const auto& error : errors) {
            if (error.find("name") != std::string::npos || error.find("empty") != std::string::npos) {
                found_name_error = true;
                break;
            }
        }
        REQUIRE(found_name_error);
    }
}

TEST_CASE("MCPPromptConfig: isSameEndpoint comparison", "[mcp][prompts][config]") {
    SECTION("Two prompts with same name are the same endpoint") {
        auto endpoint1 = MCPPromptConfigTestHelper::createPromptEndpoint("same_name", "Desc 1", "Content 1");
        auto endpoint2 = MCPPromptConfigTestHelper::createPromptEndpoint("same_name", "Desc 2", "Content 2");

        REQUIRE(endpoint1.isSameEndpoint(endpoint2) == true);
    }

    SECTION("Two prompts with different names are different endpoints") {
        auto endpoint1 = MCPPromptConfigTestHelper::createPromptEndpoint("name_a", "Desc", "Content");
        auto endpoint2 = MCPPromptConfigTestHelper::createPromptEndpoint("name_b", "Desc", "Content");

        REQUIRE(endpoint1.isSameEndpoint(endpoint2) == false);
    }

    SECTION("Prompt and tool with same name are different endpoints") {
        auto prompt = MCPPromptConfigTestHelper::createPromptEndpoint("same_name", "Desc", "Content");
        auto tool = MCPPromptConfigTestHelper::createMCPToolEndpoint("same_name");

        REQUIRE(prompt.isSameEndpoint(tool) == false);
    }
}

} // namespace test
} // namespace flapi
