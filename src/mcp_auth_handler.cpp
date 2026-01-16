#include "include/mcp_auth_handler.hpp"
#include "auth_middleware.hpp"
#include "oidc_provider_presets.hpp"
#include <crow/logging.h>
#include <crow/utility.h>
#include <jwt-cpp/jwt.h>
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

    if (mcp_auth.type == "basic") {
        return authenticateBasic(auth_header);
    } else if (mcp_auth.type == "bearer") {
        return authenticateBearer(auth_header);
    } else if (mcp_auth.type == "oidc") {
        return authenticateOIDC(auth_header);
    }

    CROW_LOG_WARNING << "Unknown MCP auth type: " << mcp_auth.type;
    return std::nullopt;
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
            if (AuthMiddleware::verifyPassword(password, user.password)) {
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
        // Decode and verify JWT token
        auto decoded = jwt::decode(token);

        // Verify JWT signature using configured secret and issuer
        auto verifier = jwt::verify()
            .allow_algorithm(jwt::algorithm::hs256{mcp_auth.jwt_secret})
            .with_issuer(mcp_auth.jwt_issuer);

        verifier.verify(decoded);

        // Extract claims from verified token
        MCPSession::AuthContext ctx;
        ctx.authenticated = true;
        ctx.username = decoded.get_payload_claim("sub").as_string();
        ctx.auth_type = "bearer";
        ctx.auth_time = std::chrono::steady_clock::now();

        // Extract roles if present in token
        try {
            auto roles_claim = decoded.get_payload_claim("roles");
            if (roles_claim.get_type() != jwt::json::type::array) {
                CROW_LOG_DEBUG << "JWT 'roles' claim is not an array, treating as empty";
            } else {
                auto roles = roles_claim.as_array();
                for (const auto& role : roles) {
                    ctx.roles.push_back(role.get<std::string>());
                }
            }
        } catch (const std::exception& e) {
            CROW_LOG_DEBUG << "No roles claim found in JWT: " << e.what();
            // Empty roles is acceptable
        }

        CROW_LOG_INFO << "MCP Bearer token authentication successful for user: " << ctx.username;
        return ctx;
    } catch (const std::exception& e) {
        CROW_LOG_DEBUG << "JWT verification failed: " << e.what();
        return std::nullopt;
    }
}

std::shared_ptr<OIDCAuthHandler> MCPAuthHandler::getOIDCHandler(const OIDCConfig& oidc_config) {
    // Make a mutable copy to apply presets
    OIDCConfig config = oidc_config;

    // Apply provider presets if specified
    if (!config.provider_type.empty() && config.provider_type != "generic") {
        bool preset_applied = OIDCProviderPresets::applyPreset(config);
        if (preset_applied) {
            CROW_LOG_DEBUG << "Applied OIDC preset for provider: " << config.provider_type;
        }
    }

    // Validate provider configuration
    std::string validation_error = OIDCProviderPresets::validateProviderConfig(config);
    if (!validation_error.empty()) {
        CROW_LOG_ERROR << "OIDC configuration error: " << validation_error;
        // Note: In production, should throw or return nullopt, but for now log and continue
        // The handler will fail during token validation with clearer errors
    }

    auto key = config.issuer_url + ":" + config.client_id;

    auto it = oidc_handlers.find(key);
    if (it != oidc_handlers.end()) {
        return it->second;
    }

    // Convert OIDCConfig to OIDCAuthHandler::Config
    OIDCAuthHandler::Config handler_config;
    handler_config.issuer_url = config.issuer_url;
    handler_config.client_id = config.client_id;
    handler_config.client_secret = config.client_secret;
    handler_config.allowed_audiences = config.allowed_audiences;
    handler_config.verify_expiration = config.verify_expiration;
    handler_config.clock_skew_seconds = config.clock_skew_seconds;
    handler_config.username_claim = config.username_claim;
    handler_config.email_claim = config.email_claim;
    handler_config.roles_claim = config.roles_claim;
    handler_config.groups_claim = config.groups_claim;
    handler_config.role_claim_path = config.role_claim_path;
    handler_config.enable_client_credentials = config.enable_client_credentials;
    handler_config.enable_refresh_tokens = config.enable_refresh_tokens;
    handler_config.scopes = config.scopes;
    handler_config.jwks_cache_hours = config.jwks_cache_hours;

    // Create new handler for this issuer
    auto handler = std::make_shared<OIDCAuthHandler>(handler_config);
    oidc_handlers[key] = handler;
    return handler;
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
    ctx.roles = claims->roles;
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
