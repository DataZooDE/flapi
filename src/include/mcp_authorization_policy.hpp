#pragma once

#include <optional>
#include <string>
#include <vector>

namespace flapi {

struct EndpointConfig;

class MCPAuthorizationPolicy {
public:
    struct Decision {
        bool allowed = false;
        std::string reason;
    };

    Decision authorize(const EndpointConfig& tool,
                       const std::vector<std::string>& user_roles,
                       bool mcp_auth_enabled) const;

    // Generic role gate reused by resources and prompts (mcp-resource /
    // mcp-prompt have no dedicated policy of their own). Semantics match the
    // per-tool check: auth disabled → allow; auth enabled + no allowed-roles
    // configured → deny-by-default; otherwise allow iff the caller holds one
    // of the allowed roles. `entity_label` is used only in the reason string
    // (e.g. "Resource 'customer_schema'").
    Decision authorizeRoles(const std::optional<std::vector<std::string>>& allowed_roles,
                            const std::string& entity_label,
                            const std::vector<std::string>& user_roles,
                            bool mcp_auth_enabled) const;
};

} // namespace flapi
