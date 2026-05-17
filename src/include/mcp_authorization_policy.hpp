#pragma once

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
};

} // namespace flapi
