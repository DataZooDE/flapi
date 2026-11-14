#include "security_auditor.hpp"

#include "redaction.hpp"

#include <algorithm>
#include <cctype>

#include "config_manager.hpp"
#include "password_hasher.hpp"

namespace flapi {

namespace {

// How to produce a hash flAPI verifies. flAPI ships no hashing command, so
// point at passlib, whose pbkdf2_sha256 output PasswordHasher accepts as is.
constexpr const char* kHashAdvice =
    "Store a PBKDF2-SHA256 hash instead, e.g. "
    "python3 -c \"from passlib.hash import pbkdf2_sha256; import getpass; "
    "print(pbkdf2_sha256.using(rounds=600000).hash(getpass.getpass()))\"";

void scanUsers(const std::vector<AuthUser>& users,
               const std::string& location,
               std::vector<SecurityWarning>& out) {
    for (const auto& user : users) {
        const std::string code = SecurityAuditor::classifyPassword(user.password);
        if (code == "AUTH_PLAINTEXT_PASSWORD") {
            out.push_back({
                code,
                "User '" + user.username + "' has a plaintext password. " + kHashAdvice + ".",
                location
            });
        } else if (code == "AUTH_MD5_PASSWORD") {
            out.push_back({
                code,
                "User '" + user.username + "' has an MD5-hashed password. "
                "MD5 is cryptographically broken. " + kHashAdvice + ".",
                location
            });
        } else if (code == "AUTH_BCRYPT_UNSUPPORTED") {
            // flAPI recognises bcrypt but cannot verify it (PasswordHasher
            // refuses rather than fail open), so this user can never log in.
            out.push_back({
                code,
                "User '" + user.username + "' has a bcrypt password hash, which flAPI does "
                "not verify: every login as this user is refused. " + kHashAdvice + ".",
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
    // The SAME classifier the verifier uses. The auditor had its own, which
    // knew bcrypt and MD5 but not PBKDF2 - so the recommended format was
    // reported as a plaintext password, and bcrypt (which the verifier
    // refuses) was reported as fine.
    switch (PasswordHasher::classifyFormat(password)) {
        case PasswordFormat::Pbkdf2Sha256:
            return {};
        case PasswordFormat::BcryptUnsupported:
            return "AUTH_BCRYPT_UNSUPPORTED";
        case PasswordFormat::Md5Deprecated:
            return "AUTH_MD5_PASSWORD";
        case PasswordFormat::PlaintextDeprecated:
            return "AUTH_PLAINTEXT_PASSWORD";
    }
    return "AUTH_PLAINTEXT_PASSWORD";
}

std::vector<SecurityWarning> SecurityAuditor::audit(const ConfigManager& config) const {
    std::vector<SecurityWarning> warnings;

    // The top-level `auth:` block, once. Endpoints that inherit it carry a
    // copy of it and are skipped below, so one bad global user is reported
    // once rather than once per endpoint.
    if (config.isAuthEnabled()) {
        const auto& global = config.getGlobalAuthConfig();
        scanUsers(global.users, "auth", warnings);
        if (global.type == "bearer" && global.jwt_secret.empty()) {
            warnings.push_back({
                "AUTH_EMPTY_JWT_SECRET",
                "The global auth block declares bearer auth with an EMPTY jwt-secret, so "
                "every token would verify. This usually means the environment variable "
                "it interpolates is unset. Authentication is refused at runtime until a "
                "secret is configured.",
                "auth"
            });
        }
    }

    for (const auto& endpoint : *config.getEndpoints()) {
        if (endpoint.auth.inherited) {
            continue;
        }
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

    // A wildcard CORS origin on an authenticated server lets any web page make
    // cross-origin requests to it and read the responses whenever the page can
    // present a credential (a token it holds, or one a user pastes). The
    // allowlist is empty by default, and empty means "*" (cors_policy.cpp).
    {
        const auto& origins = config.getCorsConfig().allow_origins;
        const bool wildcard = origins.empty() ||
            std::find(origins.begin(), origins.end(), "*") != origins.end();
        bool any_auth = config.isAuthEnabled() || mcp.auth.enabled;
        for (const auto& endpoint : *config.getEndpoints()) {
            any_auth = any_auth || endpoint.auth.enabled;
        }
        if (wildcard && any_auth) {
            warnings.push_back({
                "CORS_WILDCARD_WITH_AUTH",
                std::string("CORS allows every origin (") +
                    (origins.empty() ? "no cors.allow-origins set, which defaults to \"*\""
                                     : "cors.allow-origins contains \"*\"") +
                    ") while authentication is enabled. Any web page can call this "
                    "server cross-origin with a credential it holds and read the response. "
                    "List the origins that may call it in cors.allow-origins.",
                "cors"
            });
        }
    }

    // A credential too short to scrub makes the server withhold text rather
    // than risk disclosing it - error messages and dry-run previews alike -
    // and there is nothing in the response to say why. Say it once, at
    // startup, naming the KEY and never the value.
    for (const auto& [conn_name, conn] : config.getConnections()) {
        for (const auto& [key, value] : conn.properties) {
            if (!value.empty() && value.size() < 4 && isCredentialKey(key)) {
                warnings.push_back({
                    "CREDENTIAL_TOO_SHORT_TO_REDACT",
                    "Connection property '" + key + "' holds a credential shorter than "
                    "4 characters. Such a value cannot be redacted from rendered SQL "
                    "without corrupting the text around it, so flAPI withholds error "
                    "detail and dry-run previews for endpoints using this connection. "
                    "Use a longer secret.",
                    "connections." + conn_name
                });
            }
        }
    }

    return warnings;
}

} // namespace flapi
