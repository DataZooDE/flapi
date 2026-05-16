#include <catch2/catch_test_macros.hpp>

#include "config_manager.hpp"
#include "mcp_authorization_policy.hpp"
#include "mcp_tool_handler.hpp"

namespace flapi {
namespace test {

namespace {

EndpointConfig makeToolEndpoint(const std::string& name,
                                std::optional<std::vector<std::string>> allowed_roles = std::nullopt) {
    EndpointConfig endpoint;
    EndpointConfig::MCPToolInfo info;
    info.name = name;
    info.description = "Test tool";
    info.allowed_roles = std::move(allowed_roles);
    endpoint.mcp_tool = std::move(info);
    return endpoint;
}

} // namespace

TEST_CASE("MCPAuthorizationPolicy: server auth disabled allows any caller",
          "[security][mcp][rbac]") {
    MCPAuthorizationPolicy policy;
    auto tool = makeToolEndpoint("anything");
    auto decision = policy.authorize(tool, /*user_roles=*/{}, /*mcp_auth_enabled=*/false);

    REQUIRE(decision.allowed);
    REQUIRE(decision.reason.empty());
}

TEST_CASE("MCPAuthorizationPolicy: server auth disabled allows even with declared roles",
          "[security][mcp][rbac]") {
    MCPAuthorizationPolicy policy;
    auto tool = makeToolEndpoint("anything", std::vector<std::string>{"admin"});
    auto decision = policy.authorize(tool, {"reader"}, /*mcp_auth_enabled=*/false);

    REQUIRE(decision.allowed);
}

TEST_CASE("MCPAuthorizationPolicy: auth enabled and no allowed-roles denies by default",
          "[security][mcp][rbac]") {
    MCPAuthorizationPolicy policy;
    auto tool = makeToolEndpoint("sensitive");  // no allowed_roles
    auto decision = policy.authorize(tool, {"admin", "user"}, /*mcp_auth_enabled=*/true);

    REQUIRE_FALSE(decision.allowed);
    REQUIRE_FALSE(decision.reason.empty());
    REQUIRE(decision.reason.find("allowed-roles") != std::string::npos);
}

TEST_CASE("MCPAuthorizationPolicy: auth enabled and matching role allows",
          "[security][mcp][rbac]") {
    MCPAuthorizationPolicy policy;
    auto tool = makeToolEndpoint("query", std::vector<std::string>{"analyst", "admin"});
    auto decision = policy.authorize(tool, {"analyst"}, /*mcp_auth_enabled=*/true);

    REQUIRE(decision.allowed);
}

TEST_CASE("MCPAuthorizationPolicy: auth enabled and user has multiple roles, one matches, allows",
          "[security][mcp][rbac]") {
    MCPAuthorizationPolicy policy;
    auto tool = makeToolEndpoint("query", std::vector<std::string>{"admin"});
    auto decision = policy.authorize(tool, {"user", "admin", "guest"}, /*mcp_auth_enabled=*/true);

    REQUIRE(decision.allowed);
}

TEST_CASE("MCPAuthorizationPolicy: auth enabled and no matching role denies",
          "[security][mcp][rbac]") {
    MCPAuthorizationPolicy policy;
    auto tool = makeToolEndpoint("query", std::vector<std::string>{"admin"});
    auto decision = policy.authorize(tool, {"user", "guest"}, /*mcp_auth_enabled=*/true);

    REQUIRE_FALSE(decision.allowed);
    REQUIRE(decision.reason.find("admin") != std::string::npos);
}

TEST_CASE("MCPAuthorizationPolicy: auth enabled and empty user roles denies when roles required",
          "[security][mcp][rbac]") {
    MCPAuthorizationPolicy policy;
    auto tool = makeToolEndpoint("query", std::vector<std::string>{"admin"});
    auto decision = policy.authorize(tool, /*user_roles=*/{}, /*mcp_auth_enabled=*/true);

    REQUIRE_FALSE(decision.allowed);
}

TEST_CASE("MCPAuthorizationPolicy: auth enabled and explicitly empty allowed-roles denies all",
          "[security][mcp][rbac]") {
    // An explicit empty list is the strict reading: "no role passes this gate."
    // Operators who want anonymous access should keep mcp.auth.enabled: false.
    MCPAuthorizationPolicy policy;
    auto tool = makeToolEndpoint("query", std::vector<std::string>{});
    auto decision = policy.authorize(tool, {"admin"}, /*mcp_auth_enabled=*/true);

    REQUIRE_FALSE(decision.allowed);
}

TEST_CASE("MCPAuthorizationPolicy: role matching is case-sensitive",
          "[security][mcp][rbac]") {
    MCPAuthorizationPolicy policy;
    auto tool = makeToolEndpoint("query", std::vector<std::string>{"Admin"});
    auto decision = policy.authorize(tool, {"admin"}, /*mcp_auth_enabled=*/true);

    REQUIRE_FALSE(decision.allowed);
}

TEST_CASE("MCPToolHandler::parseRolesFromContext: empty context yields empty roles",
          "[security][mcp][rbac]") {
    std::unordered_map<std::string, std::string> context;
    REQUIRE(MCPToolHandler::parseRolesFromContext(context).empty());
}

TEST_CASE("MCPToolHandler::parseRolesFromContext: comma-separated list splits correctly",
          "[security][mcp][rbac]") {
    std::unordered_map<std::string, std::string> context{
        {MCPToolCallRequest::kRolesContextKey, "admin,analyst,reader"}
    };
    auto roles = MCPToolHandler::parseRolesFromContext(context);
    REQUIRE(roles.size() == 3);
    REQUIRE(roles[0] == "admin");
    REQUIRE(roles[1] == "analyst");
    REQUIRE(roles[2] == "reader");
}

TEST_CASE("MCPToolHandler::parseRolesFromContext: whitespace around roles is trimmed",
          "[security][mcp][rbac]") {
    std::unordered_map<std::string, std::string> context{
        {MCPToolCallRequest::kRolesContextKey, "  admin , analyst,  reader  "}
    };
    auto roles = MCPToolHandler::parseRolesFromContext(context);
    REQUIRE(roles.size() == 3);
    REQUIRE(roles[0] == "admin");
    REQUIRE(roles[1] == "analyst");
    REQUIRE(roles[2] == "reader");
}

TEST_CASE("MCPToolHandler::parseRolesFromContext: empty fragments are skipped",
          "[security][mcp][rbac]") {
    std::unordered_map<std::string, std::string> context{
        {MCPToolCallRequest::kRolesContextKey, ",admin,,analyst,"}
    };
    auto roles = MCPToolHandler::parseRolesFromContext(context);
    REQUIRE(roles.size() == 2);
    REQUIRE(roles[0] == "admin");
    REQUIRE(roles[1] == "analyst");
}

TEST_CASE("MCPAuthorizationPolicy: non-MCP endpoint short-circuits to allowed",
          "[security][mcp][rbac]") {
    // The policy is only meaningful for MCP tools; callers should still be
    // defensive if asked about something that isn't a tool.
    MCPAuthorizationPolicy policy;
    EndpointConfig rest;
    rest.urlPath = "/rest";
    auto decision = policy.authorize(rest, {"admin"}, /*mcp_auth_enabled=*/true);

    REQUIRE(decision.allowed);
}

} // namespace test
} // namespace flapi
