#pragma once

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <crow.h>
#include "config_manager.hpp"
#include "mcp_types.hpp"
#include "oidc_auth_handler.hpp"

namespace flapi {

class MCPAuthHandler {
public:
    explicit MCPAuthHandler(std::shared_ptr<ConfigManager> config_manager);

    // Authenticate from HTTP Authorization header
    std::optional<MCPSession::AuthContext> authenticate(const crow::request& req);

    // Check if method requires authentication
    bool methodRequiresAuth(const std::string& method) const;

    // Verify session is authorized for method
    bool authorizeMethod(const std::string& method,
                        const std::optional<MCPSession::AuthContext>& auth_context) const;

private:
    std::shared_ptr<ConfigManager> config_manager_;
    std::unordered_map<std::string, std::shared_ptr<OIDCAuthHandler>> oidc_handlers;  // Cache per issuer

    std::optional<MCPSession::AuthContext> authenticateBasic(const std::string& auth_header);
    std::optional<MCPSession::AuthContext> authenticateBearer(const std::string& auth_header);
    std::optional<MCPSession::AuthContext> authenticateOIDC(const std::string& auth_header);

    // Get or create OIDC handler for given config
    std::shared_ptr<OIDCAuthHandler> getOIDCHandler(const OIDCConfig& config);
};

} // namespace flapi
