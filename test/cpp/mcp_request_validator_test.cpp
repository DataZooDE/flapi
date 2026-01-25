#include <catch2/catch_test_macros.hpp>
#include "../../src/include/mcp_request_validator.hpp"

using namespace flapi;

TEST_CASE("MCPRequestValidator::validateJsonRpcRequest enforces version and method rules", "[mcp_request_validator]") {
    MCPRequestValidator::clearValidationErrors();
    MCPRequest request;
    request.id = "1";
    request.method = "initialize";
    request.params["protocolVersion"] = "2024-11-05";

    REQUIRE(MCPRequestValidator::validateJsonRpcRequest(request));

    SECTION("Invalid JSON-RPC version is rejected") {
        request.jsonrpc = "1.0";
        REQUIRE_FALSE(MCPRequestValidator::validateJsonRpcRequest(request));
        auto errors = MCPRequestValidator::getValidationErrors();
        REQUIRE_FALSE(errors.empty());
        REQUIRE(errors[0].find("Invalid JSON-RPC version") != std::string::npos);
    }

    SECTION("Invalid method name triggers validation error") {
        request.jsonrpc = "2.0";
        request.method = "invalid method";
        REQUIRE_FALSE(MCPRequestValidator::validateJsonRpcRequest(request));
        auto errors = MCPRequestValidator::getValidationErrors();
        REQUIRE_FALSE(errors.empty());
        REQUIRE(errors[0].find("Invalid method name") != std::string::npos);
    }
}

TEST_CASE("MCPRequestValidator::validateMethodExists checks known methods", "[mcp_request_validator]") {
    MCPRequestValidator::clearValidationErrors();
    REQUIRE(MCPRequestValidator::validateMethodExists("initialize"));
    REQUIRE_FALSE(MCPRequestValidator::validateMethodExists("unknown/method"));
    auto errors = MCPRequestValidator::getValidationErrors();
    REQUIRE(errors.back().find("Method not found") != std::string::npos);
}

TEST_CASE("MCPRequestValidator::validateParamsForMethod enforces method-specific schemas", "[mcp_request_validator]") {
    MCPRequestValidator::clearValidationErrors();
    crow::json::wvalue toolParams;
    toolParams["name"] = "test_tool";
    REQUIRE(MCPRequestValidator::validateParamsForMethod("tools/call", toolParams));

    crow::json::wvalue missingName;
    REQUIRE_FALSE(MCPRequestValidator::validateParamsForMethod("tools/call", missingName));

    crow::json::wvalue initializeParams;
    initializeParams["protocolVersion"] = "2024-11-05";
    REQUIRE(MCPRequestValidator::validateParamsForMethod("initialize", initializeParams));
}

TEST_CASE("MCPRequestValidator HTTP helpers validate Accept and Content-Type headers", "[mcp_request_validator]") {
    MCPRequestValidator::clearValidationErrors();
    REQUIRE(MCPRequestValidator::validateAcceptHeader("application/json, text/event-stream"));
    REQUIRE_FALSE(MCPRequestValidator::validateAcceptHeader("application/json"));

    REQUIRE(MCPRequestValidator::validateContentType("application/json"));
    REQUIRE_FALSE(MCPRequestValidator::validateContentType("text/plain"));
}
