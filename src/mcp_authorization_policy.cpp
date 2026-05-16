#include "mcp_authorization_policy.hpp"

#include <algorithm>
#include <sstream>

#include "config_manager.hpp"

namespace flapi {

namespace {

std::string formatRoleList(const std::vector<std::string>& roles) {
    if (roles.empty()) {
        return "<none>";
    }
    std::ostringstream oss;
    for (size_t i = 0; i < roles.size(); ++i) {
        if (i > 0) {
            oss << ", ";
        }
        oss << roles[i];
    }
    return oss.str();
}

bool anyRoleMatches(const std::vector<std::string>& user_roles,
                    const std::vector<std::string>& allowed_roles) {
    for (const auto& user_role : user_roles) {
        if (std::find(allowed_roles.begin(), allowed_roles.end(), user_role) != allowed_roles.end()) {
            return true;
        }
    }
    return false;
}

} // namespace

MCPAuthorizationPolicy::Decision MCPAuthorizationPolicy::authorize(
    const EndpointConfig& tool,
    const std::vector<std::string>& user_roles,
    bool mcp_auth_enabled) const
{
    // Policy only applies to MCP tools; non-tool endpoints are handled elsewhere.
    if (!tool.isMCPTool()) {
        return {true, {}};
    }

    // When the server has MCP auth turned off, the operator has opted into
    // the open-by-default demo mode. The startup auditor warns about this;
    // honour it here so first-run experiences keep working.
    if (!mcp_auth_enabled) {
        return {true, {}};
    }

    const auto& allowed = tool.mcp_tool->allowed_roles;
    if (!allowed.has_value()) {
        return {
            false,
            "Tool '" + tool.mcp_tool->name + "' has no mcp-tool.allowed-roles "
            "configured while mcp.auth.enabled is true. Add allowed-roles to "
            "the endpoint config to expose this tool, or disable mcp.auth to "
            "allow anonymous access."
        };
    }

    if (anyRoleMatches(user_roles, *allowed)) {
        return {true, {}};
    }

    return {
        false,
        "Tool '" + tool.mcp_tool->name + "' requires one of [" +
        formatRoleList(*allowed) + "]; caller has [" +
        formatRoleList(user_roles) + "]."
    };
}

} // namespace flapi
