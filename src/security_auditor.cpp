#include "security_auditor.hpp"

#include <algorithm>
#include <cctype>

#include "config_manager.hpp"

namespace flapi {

namespace {

bool isBcryptPrefix(const std::string& password) {
    if (password.size() < 4) {
        return false;
    }
    if (password[0] != '$' || password[1] != '2' || password[3] != '$') {
        return false;
    }
    const char variant = password[2];
    return variant == 'a' || variant == 'b' || variant == 'y';
}

bool isMd5HexDigest(const std::string& password) {
    if (password.size() != 32) {
        return false;
    }
    return std::all_of(password.begin(), password.end(), [](char c) {
        return std::isxdigit(static_cast<unsigned char>(c)) != 0;
    });
}

void scanUsers(const std::vector<AuthUser>& users,
               const std::string& location,
               std::vector<SecurityWarning>& out) {
    for (const auto& user : users) {
        const std::string code = SecurityAuditor::classifyPassword(user.password);
        if (code == "AUTH_PLAINTEXT_PASSWORD") {
            out.push_back({
                code,
                "User '" + user.username + "' has a plaintext password. "
                "Use bcrypt instead (see flapii auth hash, when available).",
                location
            });
        } else if (code == "AUTH_MD5_PASSWORD") {
            out.push_back({
                code,
                "User '" + user.username + "' has an MD5-hashed password. "
                "MD5 is cryptographically broken; migrate to bcrypt.",
                location
            });
        }
    }
}

} // namespace

std::string SecurityAuditor::classifyPassword(const std::string& password) {
    if (password.empty()) {
        return {};
    }
    if (isBcryptPrefix(password)) {
        return {};
    }
    if (isMd5HexDigest(password)) {
        return "AUTH_MD5_PASSWORD";
    }
    return "AUTH_PLAINTEXT_PASSWORD";
}

std::vector<SecurityWarning> SecurityAuditor::audit(const ConfigManager& config) const {
    std::vector<SecurityWarning> warnings;

    for (const auto& endpoint : *config.getEndpoints()) {
        scanUsers(endpoint.auth.users, "endpoint " + endpoint.getIdentifier(), warnings);

        // An empty HMAC key is not a weak secret, it is no secret: a token
        // signed with the empty key verifies, so any caller can mint one
        // claiming any subject and any roles. The usual way to arrive here is
        // `jwt-secret: '{{env.API_JWT_SECRET}}'` with the variable unset,
        // which resolves to "" without complaint - so the endpoint looks
        // protected in the config and is not.
        //
        // The runtime refuses this independently (auth_middleware.cpp); this
        // warning exists so an operator learns at startup rather than from an
        // audit log entry after the fact.
        if (endpoint.auth.enabled && endpoint.auth.type == "bearer" &&
            endpoint.auth.jwt_secret.empty()) {
            warnings.push_back({
                "AUTH_EMPTY_JWT_SECRET",
                "Endpoint declares bearer auth with an EMPTY jwt-secret, so every "
                "token would verify. This usually means the environment variable "
                "it interpolates is unset. Authentication is refused at runtime "
                "until a secret is configured.",
                "endpoint " + endpoint.getIdentifier()
            });
        }

        // The dispatch in AuthMiddleware knows "basic", "bearer" and "oidc".
        // Anything else matches no branch, so the endpoint denies every
        // request - it fails closed, but silently, and a config that reads as
        // protected is in fact unusable. `type: jwt` is the common case: it is
        // what the shipped customer example uses.
        if (endpoint.auth.enabled && !endpoint.auth.type.empty() &&
            endpoint.auth.type != "basic" && endpoint.auth.type != "bearer" &&
            endpoint.auth.type != "oidc") {
            warnings.push_back({
                "AUTH_UNKNOWN_TYPE",
                "Endpoint declares auth type '" + endpoint.auth.type +
                "', which is not one of basic, bearer or oidc. No authenticator "
                "matches, so every request to this endpoint is refused with 401.",
                "endpoint " + endpoint.getIdentifier()
            });
        }
    }

    const auto& mcp = config.getMCPConfig();
    scanUsers(mcp.auth.users, "mcp.auth", warnings);

    if (mcp.enabled && !mcp.auth.enabled) {
        const bool any_mcp_tool = std::any_of(
            config.getEndpoints()->begin(), config.getEndpoints()->end(),
            [](const EndpointConfig& e) { return e.isMCPTool(); });
        if (any_mcp_tool) {
            warnings.push_back({
                "MCP_UNAUTHENTICATED_TOOLS",
                "MCP tools are exposed without authentication (mcp.auth.enabled is false). "
                "Anyone reaching the server can invoke any MCP tool. "
                "Enable mcp.auth.enabled and configure users or JWT before exposing this server.",
                "mcp"
            });
        }
    }

    return warnings;
}

} // namespace flapi
