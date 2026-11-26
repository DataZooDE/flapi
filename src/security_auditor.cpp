#include "security_auditor.hpp"

#include "redaction.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>

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

// "endpoint GET /orders": the path alone is ambiguous when GET and POST
// /orders are separate endpoints with separate auth blocks.
std::string endpointLocation(const EndpointConfig& endpoint) {
    if (!endpoint.urlPath.empty()) {
        const std::string method = endpoint.method.empty() ? "GET" : endpoint.method;
        return "endpoint " + method + " " + endpoint.urlPath;
    }
    return "endpoint " + endpoint.getIdentifier();
}

// Where an endpoint-level warning points: the YAML file, relative to the
// directory of flapi.yaml ("sqls/orders.yaml"), plus the REST route when there
// is one ("sqls/orders.yaml (GET /orders)"). "endpoint MCP tool: x" did not say
// which file to open.
std::string fileLocation(const EndpointConfig& endpoint, const ConfigManager& config) {
    if (endpoint.config_file_path.empty()) {
        return endpointLocation(endpoint);
    }
    std::string file = endpoint.config_file_path;
    const auto base = std::filesystem::path(config.getBasePath());
    if (!base.empty()) {
        const auto rel = std::filesystem::path(file).lexically_relative(base);
        if (!rel.empty() && rel.generic_string().rfind("..", 0) != 0) {
            file = rel.generic_string();
        }
    }
    if (!endpoint.urlPath.empty()) {
        const std::string method = endpoint.method.empty() ? "GET" : endpoint.method;
        return file + " (" + method + " " + endpoint.urlPath + ")";
    }
    return file;
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

std::optional<std::string> SecurityAuditor::strictRefusal(const std::vector<SecurityWarning>& warnings) {
    if (warnings.empty()) {
        return std::nullopt;
    }
    std::vector<std::string> codes;
    for (const auto& w : warnings) {
        if (std::find(codes.begin(), codes.end(), w.code) == codes.end()) {
            codes.push_back(w.code);
        }
    }
    std::string joined;
    for (const auto& code : codes) {
        joined += (joined.empty() ? "" : ", ") + code;
    }
    return "--strict: " + std::to_string(warnings.size()) + " security warning(s) (" + joined +
           "). Fix them, or run without --strict.";
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
                "The global auth block declares bearer auth with an EMPTY jwt-secret. "
                "Authentication fails closed: every token is refused until a secret is "
                "configured. This usually means the environment variable it interpolates "
                "is unset.",
                "auth"
            });
        }
    }

    for (const auto& endpoint : *config.getEndpoints()) {
        // Inherited credentials are the global block's, reported above once.
        if (endpoint.auth.inherited) {
            continue;
        }
        const std::string location = endpointLocation(endpoint);
        if (!endpoint.auth.credentials_inherited) {
            scanUsers(endpoint.auth.users, location, warnings);
        }

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
                "Endpoint declares bearer auth with an EMPTY jwt-secret. Authentication "
                "fails closed: every token is refused until a secret is configured. This "
                "usually means the environment variable it interpolates is unset.",
                location
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
                location
            });
        }
    }

    const auto& mcp = config.getMCPConfig();
    scanUsers(mcp.auth.users, "mcp.auth", warnings);

    // The MCP authenticator refuses an empty key too (mcp_auth_handler.cpp),
    // but only at request time; say it at startup like for REST.
    if (mcp.auth.enabled && mcp.auth.type == "bearer" && mcp.auth.jwt_secret.empty()) {
        warnings.push_back({
            "AUTH_EMPTY_JWT_SECRET",
            "mcp.auth declares bearer auth with an EMPTY jwt-secret. Authentication "
            "fails closed: every MCP token is refused until a secret is configured. "
            "This usually means the environment variable it interpolates is unset.",
            "mcp.auth"
        });
    }

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

    if (mcp.enabled && mcp.auth.enabled) {
        // MCPAuthorizationPolicy denies an entity with no allowed-roles to
        // EVERY caller once mcp.auth is on (deny by default). A tool that
        // reads as exposed then answers 403 to everyone; say it at startup.
        // An explicit empty list (`allowed-roles: []`) lets no one in either.
        const auto report = [&](const char* kind, const std::string& name,
                                const std::optional<std::vector<std::string>>& roles,
                                const EndpointConfig& endpoint) {
            if (roles.has_value() && !roles->empty()) {
                return;
            }
            warnings.push_back({
                "MCP_TOOL_NO_ALLOWED_ROLES",
                std::string("MCP ") + kind + " '" + name + "' has " +
                    (roles.has_value() ? "an empty allowed-roles list" : "no allowed-roles") +
                    " while mcp.auth.enabled is true, so every caller is refused (HTTP 403). "
                    "Add allowed-roles listing the roles that may use it, or disable mcp.auth "
                    "to allow anonymous access.",
                fileLocation(endpoint, config)
            });
        };
        for (const auto& endpoint : *config.getEndpoints()) {
            if (endpoint.isMCPTool()) {
                report("tool", endpoint.mcp_tool->name, endpoint.mcp_tool->allowed_roles, endpoint);
            }
            if (endpoint.isMCPResource()) {
                report("resource", endpoint.mcp_resource->name, endpoint.mcp_resource->allowed_roles, endpoint);
            }
            if (endpoint.isMCPPrompt()) {
                report("prompt", endpoint.mcp_prompt->name, endpoint.mcp_prompt->allowed_roles, endpoint);
            }
        }
    }

    if (mcp.enabled && !mcp.auth.enabled) {
        // The REST route asks for credentials; the same SQL as an MCP tool
        // does not, because MCP has its own (here: no) authentication.
        for (const auto& endpoint : *config.getEndpoints()) {
            if (endpoint.isMCPTool() && endpoint.auth.enabled) {
                const std::string block = endpoint.auth.inherited ? "the top-level auth: block"
                                                                  : "its auth: block";
                const std::string what = endpoint.urlPath.empty()
                    ? "MCP tool '" + endpoint.mcp_tool->name + "' is declared with " + block +
                          ", but an endpoint's auth: block does not apply to MCP"
                    : "MCP tool '" + endpoint.mcp_tool->name + "' runs the SQL of " +
                          (endpoint.method.empty() ? std::string("GET") : endpoint.method) + " " +
                          endpoint.urlPath + ", which " + block + " protects over REST; that "
                          "auth: block does not apply to MCP";
                warnings.push_back({
                    "MCP_TOOL_BYPASSES_REST_AUTH",
                    what + ", and mcp.auth.enabled is false: any caller reaching /mcp/jsonrpc "
                    "gets the same data without credentials. Enable mcp.auth with allowed-roles, "
                    "or remove the mcp-tool block.",
                    fileLocation(endpoint, config)
                });
            }
        }
    }

    // `auth` twice in one endpoint file (two blocks, or an
    // `{{include:... as auth}}` beside a written one): the first occurrence
    // wins without a word, so the block the operator meant can be the one
    // ignored - and with it the protection.
    for (const auto& endpoint : *config.getEndpoints()) {
        if (std::find(endpoint.duplicate_keys.begin(), endpoint.duplicate_keys.end(), "auth") ==
            endpoint.duplicate_keys.end()) {
            continue;
        }
        warnings.push_back({
            "ENDPOINT_DUPLICATE_AUTH_KEY",
            "Endpoint file has the `auth` key more than once (after include expansion). Only the "
            "first one is used; the others, including any `{{include:... as auth}}` after it, are "
            "ignored. Keep exactly one `auth:` block.",
            fileLocation(endpoint, config)
        });
    }

    // An `authentication:` (or `Auth:`, `auth_config:`) key is read by
    // nothing. Without an `auth:` block the endpoint is open, or inherits the
    // top-level auth, while its file reads as protected.
    for (const auto& endpoint : *config.getEndpoints()) {
        if (endpoint.auth.declared) {
            continue;
        }
        std::string names;
        for (const auto& key : endpoint.unknown_keys) {
            std::string lower = key;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (lower.rfind("auth", 0) == 0) {
                names += (names.empty() ? "'" : ", '") + key + "'";
            }
        }
        if (names.empty()) {
            continue;
        }
        warnings.push_back({
            "ENDPOINT_AUTH_KEY_IGNORED",
            "Endpoint file has the key(s) " + names + ", which flAPI does not read, and no `auth:` "
            "block. " + (endpoint.auth.inherited
                             ? std::string("The endpoint uses the top-level auth block instead.")
                             : std::string("The endpoint requires no authentication.")) +
            " Rename the key to `auth:` if it was meant to protect this endpoint.",
            fileLocation(endpoint, config)
        });
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
