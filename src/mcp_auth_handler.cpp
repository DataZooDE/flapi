#include "include/mcp_auth_handler.hpp"
#include "auth_middleware.hpp"
#include "auth_params.hpp"
#include "bearer_token_verifier.hpp"
#include "oidc_handler_registry.hpp"
#include "oidc_provider_presets.hpp"
#include <crow/logging.h>
#include <crow/utility.h>
#include <sstream>

namespace flapi {

MCPAuthHandler::MCPAuthHandler(std::shared_ptr<ConfigManager> config_manager)
    : config_manager_(config_manager) {}

std::optional<MCPSession::AuthContext> MCPAuthHandler::authenticate(const crow::request& req) {
    const auto& mcp_auth = config_manager_->getMCPConfig().auth;
    if (!mcp_auth.enabled) {
        return std::nullopt;
    }

    auto auth_header = req.get_header_value("Authorization");
    if (auth_header.empty()) {
        CROW_LOG_DEBUG << "MCP auth required but no Authorization header provided";
        return std::nullopt;
    }

    std::optional<MCPSession::AuthContext> authenticated;
    if (mcp_auth.type == "basic") {
        authenticated = authenticateBasic(auth_header);
    } else if (mcp_auth.type == "bearer") {
        authenticated = authenticateBearer(auth_header);
    } else if (mcp_auth.type == "oidc") {
        authenticated = authenticateOIDC(auth_header);
    } else {
        CROW_LOG_WARNING << "Unknown MCP auth type: " << mcp_auth.type;
        return std::nullopt;
    }

    // An identity with a control character, or a role containing a comma, is
    // refused, as on REST (AuthMiddleware). See auth_params.hpp.
    if (authenticated) {
        if (const char* refusal =
                IdentityRefusal(authenticated->username, authenticated->email, authenticated->roles,
                                authenticated->groups)) {
            CROW_LOG_WARNING << "MCP " << mcp_auth.type << " authentication refused: " << refusal;
            return std::nullopt;
        }
    }
    return authenticated;
}

bool MCPAuthHandler::methodRequiresAuth(const std::string& method) const {
    const auto& mcp_auth = config_manager_->getMCPConfig().auth;
    if (!mcp_auth.enabled) {
        return false;
    }

    // Check method-specific override
    auto it = mcp_auth.methods.find(method);
    if (it != mcp_auth.methods.end()) {
        return it->second.required;
    }

    // Default: require auth if protocol auth enabled
    return true;
}

bool MCPAuthHandler::authorizeMethod(
    const std::string& method,
    const std::optional<MCPSession::AuthContext>& auth_context) const {

    if (!methodRequiresAuth(method)) {
        return true;  // Method doesn't require auth
    }

    // Method requires auth - check if session is authenticated
    return auth_context && auth_context->authenticated;
}

std::optional<MCPSession::AuthContext> MCPAuthHandler::authenticateBasic(
    const std::string& auth_header) {

    // Check for "Basic " prefix
    if (auth_header.substr(0, 6) != "Basic ") {
        CROW_LOG_DEBUG << "Authorization header does not start with 'Basic '";
        return std::nullopt;
    }

    // Decode base64 (simplified - in production would use proper base64 library)
    std::string encoded = auth_header.substr(6);

    // Use Crow's base64 decoding utility
    std::string decoded = crow::utility::base64decode(encoded);
    size_t colon_pos = decoded.find(':');
    if (colon_pos == std::string::npos) {
        CROW_LOG_DEBUG << "Invalid Basic auth format: missing colon separator";
        return std::nullopt;
    }

    std::string username = decoded.substr(0, colon_pos);
    std::string password = decoded.substr(colon_pos + 1);

    const auto& mcp_auth = config_manager_->getMCPConfig().auth;

    // Validate against configured users (reusing REST pattern)
    for (const auto& user : mcp_auth.users) {
        if (user.username == username) {
            // Use AuthMiddleware's password verification (supports MD5 hashing)
            if (AuthMiddleware::verifyCredentials(username, password, user.password)) {
                MCPSession::AuthContext ctx;
                ctx.authenticated = true;
                ctx.username = username;
                ctx.roles = user.roles;
                ctx.auth_type = "basic";
                ctx.auth_time = std::chrono::steady_clock::now();
                CROW_LOG_INFO << "MCP Basic authentication successful for user: " << username;
                return ctx;
            }
        }
    }

    CROW_LOG_WARNING << "MCP Basic authentication failed for user: " << username;
    return std::nullopt;
}

std::optional<MCPSession::AuthContext> MCPAuthHandler::authenticateBearer(
    const std::string& auth_header) {

    // Check for "Bearer " prefix
    if (auth_header.substr(0, 7) != "Bearer ") {
        CROW_LOG_DEBUG << "Authorization header does not start with 'Bearer '";
        return std::nullopt;
    }

    std::string token = auth_header.substr(7);
    const auto& mcp_auth = config_manager_->getMCPConfig().auth;

    try {
        // An empty HMAC key is not a weak secret, it is no secret: anyone can
        // sign a token with the empty key and it verifies, claiming any
        // subject and any roles. `jwt-secret: '{{env.MCP_JWT_SECRET}}'`
        // resolves to "" when the variable is unset, silently.
        //
        // The REST path refuses this (auth_middleware.cpp). This one did not,
        // so the same bypass remained open over MCP - and MCP is the surface
        // agents call.
        if (mcp_auth.jwt_secret.empty()) {
            CROW_LOG_ERROR << "Refusing MCP bearer authentication: jwt-secret is empty, "
                              "so any token would verify. Check that the environment "
                              "variable it interpolates is set.";
            return std::nullopt;
        }

        BearerVerifierConfig verifier;
        verifier.secret = mcp_auth.jwt_secret;
        verifier.issuer = mcp_auth.jwt_issuer;
        verifier.audiences = mcp_auth.jwt_audiences;
        verifier.require_exp = mcp_auth.jwt_require_exp;
        std::string reason;
        auto identity = VerifyBearerToken(token, verifier, &reason);
        if (!identity) {
            CROW_LOG_DEBUG << "JWT verification failed: " << reason;
            return std::nullopt;
        }

        MCPSession::AuthContext ctx;
        ctx.authenticated = true;
        ctx.username = std::move(identity->username);
        ctx.email = std::move(identity->email);
        ctx.roles = std::move(identity->roles);
        ctx.auth_type = "bearer";
        ctx.auth_time = std::chrono::steady_clock::now();

        CROW_LOG_INFO << "MCP Bearer token authentication successful for user: " << ctx.username;
        return ctx;
    } catch (const std::exception& e) {
        CROW_LOG_DEBUG << "JWT verification failed: " << e.what();
        return std::nullopt;
    }
}

std::shared_ptr<OIDCAuthHandler> MCPAuthHandler::getOIDCHandler(const OIDCConfig& oidc_config) {
    // One locked registry for REST and MCP, keyed on the whole effective
    // config. The provider config was validated at load (parseOIDCConfigNode).
    return OIDCHandlerRegistry::instance().handlerFor(oidc_config);
}

std::optional<MCPSession::AuthContext> MCPAuthHandler::authenticateOIDC(
    const std::string& auth_header) {

    // Check for "Bearer " prefix
    if (auth_header.substr(0, 7) != "Bearer ") {
        CROW_LOG_DEBUG << "MCP OIDC: Authorization header does not start with 'Bearer '";
        return std::nullopt;
    }

    // Ensure MCP auth has OIDC configuration
    const auto& mcp_auth = config_manager_->getMCPConfig().auth;
    if (!mcp_auth.oidc) {
        CROW_LOG_WARNING << "MCP OIDC authentication requested but no OIDC config present";
        return std::nullopt;
    }

    // Get or create OIDC handler
    auto oidc_handler = getOIDCHandler(*mcp_auth.oidc);

    // Extract and validate token
    std::string token = auth_header.substr(7);
    auto claims = oidc_handler->validateToken(token);

    if (!claims) {
        CROW_LOG_DEBUG << "MCP OIDC token validation failed";
        return std::nullopt;
    }

    // Set authentication context
    MCPSession::AuthContext ctx;
    ctx.authenticated = true;
    ctx.username = claims->username;
    ctx.email = claims->email;
    ctx.roles = claims->roles;
    ctx.groups = claims->groups;
    ctx.auth_type = "oidc";
    ctx.auth_time = std::chrono::steady_clock::now();

    // Store OIDC-specific fields
    ctx.bound_token_jti = claims->jti;  // Bind token to session
    ctx.token_expires_at = claims->expires_at;
    // Note: refresh_token would be populated if client credentials flow is used

    CROW_LOG_INFO << "MCP OIDC authentication successful for user: " << ctx.username;
    return ctx;
}

} // namespace flapi
