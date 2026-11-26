#pragma once

#include <functional>

#include <crow.h>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <iostream>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <yaml-cpp/yaml.h>

#include "audit_logger.hpp"
#include "trace_capture_policy.hpp"
#include "tracing_config.hpp"
#include "route_translator.hpp"
#include "extended_yaml_parser.hpp"
#include "path_utils.hpp"

namespace flapi {

struct TimeInterval {
    static std::optional<std::chrono::seconds> parseInterval(const std::string& interval);
    // A maintenance schedule (ducklake.compaction.schedule): an interval as parseInterval
    // takes it, or one of the shorthands @hourly, @daily, @weekly.
    static std::optional<std::chrono::seconds> parseSchedule(const std::string& schedule);
};

struct ConnectionConfig {
    std::string init;
    std::unordered_map<std::string, std::string> properties;
    bool log_queries = false;
    bool log_parameters = false;
    std::string allow;

    // Serialise ALL access to this connection - reads included.
    //
    // A concurrent read and write against a DuckDB SQLite attachment deadlock
    // each other: two requests are enough, and the attachment then never
    // answers again, while the process, DuckDB and every other connection stay
    // healthy (#116). Serialising only the writes does not help - the pair that
    // wedges is a reader and a writer.
    //
    // Auto-detected from an `ATTACH ... (TYPE sqlite)` in `init`, and settable
    // explicitly for a backend we do not recognise, or to opt out of the
    // serialisation for one we do:
    //
    //   connections:
    //     mydb:
    //       serialize-access: true
    std::optional<bool> serialize_access;

    const std::string& getInit() const { return init; }
    void setInit(const std::string& initSql) { init = initSql; }

    /// True when every query on this connection must be serialised.
    bool serialisesAccess() const;
};

struct HeartbeatConfig {
    bool enabled = false;
    std::unordered_map<std::string, std::string> params;
};

struct RateLimitConfig {
    // True once an endpoint actually declared a `rate-limit:` block. Needed to
    // tell "did not mention it" from "explicitly switched it off": both leave
    // `enabled` false, and the global block applies only to the former. Without
    // this, `rate-limit: {enabled: false}` would silently inherit the global
    // limit - the opposite of what it says.
    bool declared = false;
    bool enabled = false;
    int max = 100;
    int interval = 60;
    // W1.4: how requests are grouped into buckets. "ip" preserves the
    // historic default; "user" / "user-or-ip" enable per-principal limits.
    std::string key_strategy = "ip";
};

struct AuthUser {
    std::string username;
    std::string password;
    std::vector<std::string> roles;
};

struct AuthFromSecretManagerConfig {
    std::string secret_name;
    std::string region;
    std::string secret_id;
    std::string secret_key;
    // Accepted for old configs and ignored (a warning says so): the fetched user
    // list lives in AuthMiddleware's memory, not in a table templates can read.
    std::string secret_table;
    // Legacy: SQL creating a DuckDB S3 secret named secretNameToSecretId(secret_name).
    // Run in a private DuckDB instance, never the main one. Set only when the
    // config has an `init:` key (init_declared), even an empty one.
    std::string init;
    bool init_declared = false;

    const std::string& getInit() const { return init; }
    void setInit(const std::string& initSql) { init = initSql; }
};

struct OIDCConfig {
    // Basic OIDC settings
    std::string issuer_url;                           // e.g., https://accounts.google.com
    std::string client_id;                            // OAuth 2.0 client ID
    std::string client_secret;                        // Client secret (from env if needed)

    // Provider type for presets
    std::string provider_type = "generic";            // "google", "microsoft" (alias "azure"), "keycloak", etc.

    // Values substituted into the preset issuer-url placeholders:
    // {tenant} <- tenant-id (Microsoft), {realm} <- realm (Keycloak),
    // {domain} <- domain (Keycloak host, Auth0, Okta).
    std::string tenant_id;
    std::string realm;
    std::string domain;

    // Token validation
    std::vector<std::string> allowed_audiences;       // Expected 'aud' claims
    bool verify_expiration = true;
    int clock_skew_seconds = 300;                     // 5 minute tolerance

    // Claim mapping
    std::string username_claim = "sub";               // Which claim contains username
    // `username-claim` was written in the config. A preset then leaves it
    // alone: `username-claim: sub` under `provider-type: keycloak` used to be
    // replaced by the preset's preferred_username, because the preset could
    // not tell "sub" written from "sub" defaulted.
    bool username_claim_explicit = false;
    std::string email_claim = "email";
    std::string roles_claim = "roles";
    std::string groups_claim = "groups";

    // Nested claim paths (for realm_access.roles style)
    std::string role_claim_path;                      // "realm_access.roles" for Keycloak

    // OAuth flows
    bool enable_client_credentials = false;
    bool enable_refresh_tokens = false;
    std::vector<std::string> scopes;

    // JWKS caching
    int jwks_cache_hours = 24;                        // How long to cache JWKS
    // Minimum seconds between two JWKS fetches for a kid the cached set does
    // not hold (`jwks-refetch-cooldown-seconds`); 0 refetches every time.
    int jwks_refetch_cooldown_seconds = 60;
};

struct AuthConfig {
    bool enabled = false;
    std::string type;
    std::vector<AuthUser> users;
    std::optional<AuthFromSecretManagerConfig> from_aws_secretmanager;
    std::string jwt_secret;
    std::string jwt_issuer;                           // empty: `iss` is not checked (startup warns)
    std::vector<std::string> jwt_audiences;           // `jwt-audience`; empty: `aud` is not checked
    bool jwt_require_exp = true;                      // `jwt-require-exp`
    std::optional<OIDCConfig> oidc;                   // NEW: OIDC configuration
    // Roles the authenticated principal must hold (any one of them). Empty
    // means "any authenticated principal". Enforced by AuthMiddleware with 403.
    std::vector<std::string> roles;
    // The endpoint carries its own `auth:` block (YAML) / `auth` object (JSON).
    // An endpoint WITHOUT one inherits the top-level `auth:` block.
    bool declared = false;
    // This config was copied from the top-level `auth:` block rather than
    // written on the endpoint. Serialisers leave it out, so persisting an
    // endpoint does not freeze the global credentials into its file.
    bool inherited = false;
    // The endpoint declares its own block but only roles (or other non-
    // credential keys): type and credentials come from the top-level block
    // (ConfigManager::inheritGlobalCredentials). Serialisers then write the
    // roles but not the inherited type.
    bool credentials_inherited = false;
};

struct ValidatorConfig {
    std::string type;
    std::string regex;
    int min = 0;
    int max = 0;
    // Bounds of a `number` (float/double) validator. Optional because 0 is a
    // real bound: unset must stay distinguishable from `min: 0`.
    std::optional<double> minNumber;
    std::optional<double> maxNumber;
    std::string minDate;
    std::string maxDate;
    std::string minTime;
    std::string maxTime;
    std::vector<std::string> allowedValues;
    bool preventSqlInjection = true; // New field for SQL injection prevention
};

struct RequestFieldConfig {
    std::string fieldName;
    std::string fieldIn;
    std::string description;
    bool required = false;
    std::string defaultValue;
    std::vector<ValidatorConfig> validators;

    // MCP 2026-07-28 x-mcp-header: when set, this parameter is mirrored by the
    // client into an `Mcp-Param-<name>` HTTP header so an edge proxy can route
    // and rate-limit on it without parsing the body. The value is the header
    // suffix (e.g. "Tenant" -> Mcp-Param-Tenant). Never annotate a secret — the
    // value is visible to every intermediary.
    std::string mcp_header;
};

// When a cache is (re)built at startup. `always` is the historic behaviour; `if-empty`
// skips a table that already exists in the catalog; `never` leaves building to the
// scheduler, a manual refresh or `flapi cache refresh`.
enum class CacheWarmupMode {
    Always,
    IfEmpty,
    Never
};

std::optional<CacheWarmupMode> ParseCacheWarmupMode(const std::string& value);
std::string CacheWarmupModeToString(CacheWarmupMode mode);

struct CacheConfig {
    bool enabled = false;
    std::string table;
    std::string schema = "cache";
    std::optional<std::string> schedule;
    std::vector<std::string> primary_keys;
    struct CursorConfig {
        std::string column;
        std::string type;
    };
    std::optional<CursorConfig> cursor;
    std::optional<std::string> rollback_window;
    struct RetentionConfig {
        std::optional<std::size_t> keep_last_snapshots;
        std::optional<std::string> max_snapshot_age;
    } retention;
    std::optional<std::string> delete_handling;
    std::optional<std::string> template_file;
    bool invalidate_on_write = false;  // Invalidate cache after write operations
    bool refresh_on_write = false;      // Refresh cache immediately after write operations
    std::optional<CacheWarmupMode> warmup;  // Overrides ducklake.warmup for this endpoint

    bool hasCursor() const { return cursor.has_value(); }
    bool hasPrimaryKey() const { return !primary_keys.empty(); }
    bool hasTemplate() const { return template_file.has_value(); }
    std::chrono::seconds getRefreshTimeInSeconds() const;
};

struct OperationConfig {
    enum Type { Read, Write };
    Type type = Type::Read;  // Default to read for backward compatibility
    bool returns_data = false;  // For RETURNING clauses in INSERT/UPDATE
    bool transaction = true;  // Wrap writes in transactions (default: safe)
    bool validate_before_write = true;  // Stricter validation for writes (default: safe)
};

struct EndpointConfig {
    std::string urlPath;
    std::string method;
    // Top-level `description:`. The REST operation's description in /doc.yaml;
    // an endpoint without one falls back to mcp-tool.description there.
    std::string description;
    bool request_fields_validation = false;
    std::vector<RequestFieldConfig> request_fields;
    std::string templateSource;
    std::vector<std::string> connection;
    bool with_pagination = true;
    // `total-count: false`: paginated responses carry no total_count (and no X-Total-Count), and
    // flAPI runs no COUNT query for them; `next` is found by reading one row past the page.
    bool total_count = true;
    // `allow-shadowing: true`: this endpoint may take precedence over a less
    // specific, overlapping route of the same method whose effective auth
    // differs in any way (scheme, roles, users, secrets, issuers), in either
    // direction (`/customers/7` open beside `/customers/:id` protected, or the
    // other way round). Without it that combination is refused at load.
    bool allow_shadowing = false;
    RateLimitConfig rate_limit;
    AuthConfig auth;
    CacheConfig cache;
    HeartbeatConfig heartbeat;
    OperationConfig operation;  // Operation configuration (read/write, transaction, etc.)
    
    // Path to the YAML configuration file for this endpoint (for reloading)
    std::string config_file_path;

    // MCP-specific metadata (optional)
    struct MCPToolInfo {
        std::string name;
        std::string description;
        std::string result_mime_type = "application/json";

        // Per-tool RBAC (W2.1). std::nullopt means "no allowed-roles configured"
        // — under MCP auth this denies by default; with MCP auth disabled the
        // policy short-circuits to allow. An explicit empty vector denies all.
        std::optional<std::vector<std::string>> allowed_roles;

        // Per-tool response shaping (W2.4). Empty / unset → no shaping,
        // result is returned as-is. See `MCPResponseShaper` for semantics.
        struct ResponseShape {
            std::optional<std::size_t> max_rows;
            std::vector<std::string> redact_columns;
            // Per-endpoint tracing capture override (mcp-tool.tracing.capture).
            // Optional so "unset" is distinguishable from "set to the global
            // default"; a global `off` still wins, whatever this says.
            std::optional<CaptureTier> tracing_capture;
            bool sample = false;
        } response;

        // W2.5: per-tool rate limit. `enabled: false` (default) leaves
        // the tool ungated; otherwise `max` calls are permitted per
        // `interval` seconds, scoped to (tool_name, principal).
        RateLimitConfig rate_limit;

        // MCP 2026-07-28 Tasks extension. `async: true` runs this tool as a
        // durable task (returns a taskId immediately) for clients that declare
        // the tasks capability. `async_after_ms > 0` runs synchronously but
        // degrades to a task if the call outruns that budget. Both default off,
        // so a tool stays fully synchronous unless opted in.
        bool async = false;
        int async_after_ms = 0;
    };

    struct MCPResourceInfo {
        std::string name;
        std::string description;
        std::string mime_type = "application/json";

        // Per-resource RBAC. Same semantics as MCPToolInfo::allowed_roles:
        // std::nullopt denies by default under mcp.auth.enabled; an explicit
        // empty vector denies all; with MCP auth disabled the check is skipped.
        std::optional<std::vector<std::string>> allowed_roles;

        // Optional RFC 6570 (simple {var} expansion) URI template, e.g.
        // "flapi://customers/{id}". When set, the resource is a parameterised
        // template: it appears in resources/templates/list, and resources/read
        // binds the path variables into request params (validated like any
        // other field) before executing. When empty the resource is a static
        // "flapi://<name>" URI.
        std::string uri_template;

        // Response shaping (mcp-resource.response), like mcp-tool.response.
        // Unset falls back to the same file's mcp-tool.response, and max-rows
        // then to kDefaultMaxRows: a resource is read whole, with no LIMIT the
        // caller can pass, so an uncapped read of a big table went to the
        // model in full (16282 rows / 3.5 MB measured).
        static constexpr std::size_t kDefaultMaxRows = 1000;
        struct ResponseShape {
            std::optional<std::size_t> max_rows;
            std::optional<std::vector<std::string>> redact_columns;
        } response;
    };

    struct MCPPromptInfo {
        std::string name;
        std::string description;
        std::string template_content;
        std::vector<std::string> arguments;

        // Per-prompt RBAC. See MCPResourceInfo::allowed_roles.
        std::optional<std::vector<std::string>> allowed_roles;
    };

    std::optional<MCPToolInfo> mcp_tool;
    std::optional<MCPResourceInfo> mcp_resource;
    std::optional<MCPPromptInfo> mcp_prompt;

    // Endpoint type enumeration
    enum class Type {
        REST,
        MCP_Tool,
        MCP_Resource,
        MCP_Prompt,
        Unknown
    };

    // Helper methods to check if this is an MCP entity
    bool isMCPEntity() const { return mcp_tool.has_value() || mcp_resource.has_value() || mcp_prompt.has_value(); }
    bool isRESTEndpoint() const { return !urlPath.empty(); }
    bool isMCPTool() const { return mcp_tool.has_value(); }
    bool isMCPResource() const { return mcp_resource.has_value(); }
    bool isMCPPrompt() const { return mcp_prompt.has_value(); }
    
    // Get the type of this endpoint
    Type getType() const {
        if (!urlPath.empty()) return Type::REST;
        if (mcp_tool.has_value()) return Type::MCP_Tool;
        if (mcp_resource.has_value()) return Type::MCP_Resource;
        if (mcp_prompt.has_value()) return Type::MCP_Prompt;
        return Type::Unknown;
    }
    
    // Get the unique name/path of this endpoint (abstracted across REST and MCP)
    std::string getName() const {
        if (!urlPath.empty()) {
            return urlPath;
        } else if (mcp_tool.has_value()) {
            return mcp_tool->name;
        } else if (mcp_resource.has_value()) {
            return mcp_resource->name;
        } else if (mcp_prompt.has_value()) {
            return mcp_prompt->name;
        }
        return "";
    }
    
    // Get the URL slug for this endpoint (centralized slugging logic)
    // REST endpoints: convert path to slug (e.g., /customers/ → customers-slash)
    // MCP entities: use name as-is (e.g., customer_lookup → customer_lookup)
    std::string getSlug() const {
        if (!urlPath.empty()) {
            return PathUtils::pathToSlug(urlPath);
        } else {
            // MCP names are already URL-safe, use as-is
            return getName();
        }
    }
    
    // Get a human-readable identifier for this endpoint (for logging/debugging)
    std::string getIdentifier() const {
        if (!urlPath.empty()) {
            return "REST endpoint: " + urlPath;
        } else if (mcp_tool.has_value()) {
            return "MCP tool: " + mcp_tool->name;
        } else if (mcp_resource.has_value()) {
            return "MCP resource: " + mcp_resource->name;
        } else if (mcp_prompt.has_value()) {
            return "MCP prompt: " + mcp_prompt->name;
        } else {
            return "unknown endpoint";
        }
    }
    
    // Get a short description for logging (more compact than getIdentifier)
    std::string getShortDescription() const {
        switch (getType()) {
            case Type::REST:
                return method + " " + urlPath;
            case Type::MCP_Tool:
                return "MCP Tool: " + mcp_tool->name;
            case Type::MCP_Resource:
                return "MCP Resource: " + mcp_resource->name;
            case Type::MCP_Prompt:
                return "MCP Prompt: " + mcp_prompt->name;
            default:
                return "unknown";
        }
    }
    
    // Check if a given path/name matches this endpoint
    bool matchesPath(const std::string& path) const;
    
    // Check if this endpoint refers to the same logical endpoint as another
    bool isSameEndpoint(const EndpointConfig& other) const {
        if (getType() != other.getType()) return false;
        
        switch (getType()) {
            case Type::REST:
                return urlPath == other.urlPath;
            case Type::MCP_Tool:
                return mcp_tool->name == other.mcp_tool->name;
            case Type::MCP_Resource:
                return mcp_resource->name == other.mcp_resource->name;
            case Type::MCP_Prompt:
                return mcp_prompt->name == other.mcp_prompt->name;
            default:
                return false;
        }
    }
    
    // Validate this endpoint configuration and return errors
    std::vector<std::string> validateSelf() const {
        std::vector<std::string> errors;
        
        switch (getType()) {
            case Type::REST:
                if (urlPath.empty()) {
                    errors.push_back("url-path cannot be empty");
                }
                if (!urlPath.empty() && urlPath[0] != '/') {
                    errors.push_back("url-path must start with /");
                } else if (!urlPath.empty()) {
                    if (auto problem = RouteTranslator::urlPathProblem(urlPath); !problem.empty()) {
                        errors.push_back(problem);
                    }
                }
                break;
            case Type::MCP_Tool:
                if (mcp_tool->name.empty()) {
                    errors.push_back("mcp-tool.name cannot be empty");
                }
                break;
            case Type::MCP_Resource:
                if (mcp_resource->name.empty()) {
                    errors.push_back("mcp-resource.name cannot be empty");
                }
                break;
            case Type::MCP_Prompt:
                if (mcp_prompt->name.empty()) {
                    errors.push_back("mcp-prompt.name cannot be empty");
                }
                break;
            case Type::Unknown:
                errors.push_back("Endpoint must define url-path, mcp-tool, mcp-resource, or mcp-prompt");
                break;
        }
        
        return errors;
    }
};

enum class EndpointJsonStyle {
    HyphenCase,
    CamelCase
};

struct MCPMethodAuthConfig {
    bool required = true;
};

struct MCPAuthConfig {
    bool enabled = false;
    std::string type = "bearer";  // "basic", "bearer", or "oidc"
    std::vector<AuthUser> users;  // For Basic auth - inline user list
    std::string jwt_secret;
    // Empty: `iss` is not checked (startup warns), as for REST. It used to
    // default to "flapi" here and to "" (meaning `iss` must be "") on REST.
    std::string jwt_issuer;
    std::vector<std::string> jwt_audiences;           // `jwt-audience`; empty: `aud` is not checked
    bool jwt_require_exp = true;                      // `jwt-require-exp`
    std::optional<OIDCConfig> oidc;  // NEW: OIDC configuration
    std::unordered_map<std::string, MCPMethodAuthConfig> methods;

    // RFC 9728 protected-resource metadata. `canonical_resource_uri` is the
    // audience the MCP endpoint identifies as; when empty it is derived at
    // runtime from the request host + the MCP path. `scopes_supported` is
    // advertised in the metadata document (optional).
    std::string canonical_resource_uri;
    std::vector<std::string> scopes_supported;
};

struct MCPConfig {
    bool enabled = true;
    MCPAuthConfig auth;
    std::string instructions;           // Inline instructions content
    std::string instructions_file;      // Path to markdown file (optional)

    // When true, the endpoint parser rejects any mcp-tool whose description
    // trips the MCPDescriptionScanner (control chars, prompt-injection
    // phrases, oversize). When false (default) the scanner warns at parse
    // time but loading still succeeds. See W2.3 in issue #24.
    bool strict_descriptions = false;

    // Page size for tools/list, resources/list, prompts/list. 0 (the default)
    // disables pagination entirely — every item is returned in one page and no
    // nextCursor is emitted, exactly as before. A positive value opts into
    // cursor-based paging.
    int page_size = 0;

    // MCP 2026-07-28 Tasks extension worker pool.
    int tasks_workers = 2;         // concurrent background workers
    int tasks_queue_depth = 32;    // max queued tasks before backpressure
    int tasks_default_ttl_ms = 3600000;   // terminal-task retention (1h)
    int tasks_poll_interval_ms = 1000;     // suggested client poll interval
};

struct DuckDBConfig {
    std::unordered_map<std::string, std::string> settings;
    std::string db_path;  // New field for database path
    std::vector<std::string> default_extensions;
    bool offline_extensions = false;  // FLAPI_EXTENSIONS_OFFLINE: never download, fail fast on a missing extension
};

// duckdb-hardening (#253): what SQL templates may reach. A top-level block, not under
// `duckdb:`, because every scalar there is handed to DuckDB as a setting.
enum class ExternalAccessMode { Auto, Full, Restricted };

struct DuckDBHardeningConfig {
    // auto = restricted when the main config file is remote, full otherwise.
    ExternalAccessMode external_access = ExternalAccessMode::Auto;
    // Absolute (relative entries resolve against the working directory, like every
    // relative path a template reads) and each ending in '/'; URLs kept as written.
    std::vector<std::string> allowed_directories;
    std::vector<std::string> allowed_paths;
};

struct TemplateConfig {
    std::string path;
    std::vector<std::string> environment_whitelist;

    bool isEnvironmentVariableAllowed(const std::string& varName) const {
        if (environment_whitelist.empty()) {
            return false;  // Empty means NONE: an unset whitelist must never mean "no protection" (#157)
        }
        for (const auto& pattern : environment_whitelist) {
            if (std::regex_match(varName, CompiledWhitelistPattern(pattern))) {
                return true;
            }
        }
        return false;
    }
};

struct HttpsConfig {
    bool enabled = false;
    std::string ssl_cert_file;
    std::string ssl_key_file;
};

// W1.2: CORS allowlist configuration. Empty fields preserve the historic
// wildcard behaviour so demo / init projects keep working in browsers.
struct CorsConfig {
    std::vector<std::string> allow_origins;   // empty → "*"; "*" → "*"; else allowlist
    std::vector<std::string> allow_headers;   // empty → "*"
    std::vector<std::string> allow_methods;   // empty → default GET/POST/PUT/PATCH/DELETE
};

struct GlobalHeartbeatConfig {
    // ONE default, whether or not a `heartbeat:` block is present. The block used to
    // default to 10 s and its absence to 60 s, so adding `heartbeat: {enabled: false}`
    // silently changed how often cache schedules were checked.
    static constexpr std::chrono::seconds kDefaultWorkerInterval{60};
    bool enabled = false;
    std::chrono::seconds workerInterval = kDefaultWorkerInterval;
};

struct DuckLakeRetentionConfig {
    std::optional<std::size_t> keep_last_snapshots;
    std::optional<std::string> max_snapshot_age;
};

struct DuckLakeCompactionConfig {
    bool enabled = false;
    std::optional<std::string> schedule;
    // ducklake.compaction.delete-threshold: the fraction of deleted rows (0 < x <= 1)
    // above which ducklake_rewrite_data_files rewrites a data file. Unset = DuckLake's
    // default (0.95), which a merge cache's delete files rarely reach.
    std::optional<double> delete_threshold;
};

struct DuckLakeSchedulerConfig {
    // ON by default: a `cache.schedule` is itself the opt-in. It used to default to off,
    // so a schedule silently never fired unless this block also said `enabled: true`.
    // `enabled: false` written in the config is honoured as an explicit opt-out.
    bool enabled = true;
    bool enabled_explicit = false;   // the config wrote `scheduler.enabled`
    std::optional<std::string> scan_interval;   // validated at load (TimeInterval)
};

struct DuckLakeConfig {
    bool enabled = false;
    std::string alias = "cache";
    std::string metadata_path;
    std::string data_path;
    bool override_data_path = false;
    bool read_only = false;  // Attach READ_ONLY; implies warm-up never and no cache maintenance
    CacheWarmupMode warmup = CacheWarmupMode::Always;
    DuckLakeRetentionConfig retention;
    DuckLakeCompactionConfig compaction;
    DuckLakeSchedulerConfig scheduler;
    std::optional<std::size_t> data_inlining_row_limit;
};

struct StorageCacheConfig {
    bool enabled = true;
    std::chrono::seconds ttl{300};          // 5 minutes default
    size_t max_size_bytes = 50UL * 1024UL * 1024UL;  // 50 MB default
};

struct StorageConfig {
    StorageCacheConfig cache;
};

// Forward declarations
class EndpointConfigParser;
class ConfigLoader;
class EndpointRepository;
class IFileProvider;

/// Mask the credentials in a connection string or DSN, wherever it appears in
/// `text`: libpq/ODBC `key=value` pairs whose key names a secret (password,
/// passwd, pwd, secret, token, api_key, ...; also `PGPASSWORD=`) and the
/// password of a `scheme://user:password@host` URL. Everything else - host,
/// database, user - is kept, so the value stays recognisable. For logs and
/// every config API answer that shows a DuckLake metadata-path or data-path.
std::string RedactConnectionSecrets(const std::string& text);

class ConfigManager {
    // Allow EndpointConfigParser to access protected parsing methods
    friend class EndpointConfigParser;

public:
    explicit ConfigManager(const std::filesystem::path& config_file);

    // Virtual destructor (defined in implementation file to handle unique_ptr cleanup)
    virtual ~ConfigManager();

    // Delete copy constructor and copy assignment (unique_ptr members are non-copyable)
    ConfigManager(const ConfigManager&) = delete;
    ConfigManager& operator=(const ConfigManager&) = delete;

    // Move semantics (defined in implementation file with complete types)
    ConfigManager(ConfigManager&&) noexcept;
    ConfigManager& operator=(ConfigManager&&) noexcept;
    
    void loadConfig();
    std::vector<std::string> listRemoteEndpointFiles(const std::string& root) const;
    void loadEndpointConfigsRecursively(const std::filesystem::path& dir);
    void loadEndpointConfig(const std::string& config_file);

    // `--validate-config` wants every invalid endpoint file, not the first: with
    // collection on, a file that fails to load is recorded (message includes the
    // file name) and loading carries on with the next one, instead of the first
    // failure aborting loadConfig(). Off by default - a server must not start with
    // a file it could not load. Set before loadConfig().
    void setCollectLoadErrors(bool collect) { collect_load_errors_ = collect; }
    const std::vector<std::string>& getLoadErrors() const { return load_errors_; }

    // Getters for configuration values
    std::string getProjectName() const;
    std::string getProjectDescription() const;
    std::string getServerName() const;
    int getHttpPort() const;
    void setHttpPort(int port);
    std::string getHttpHost() const;
    void setHttpHost(const std::string& host);
    virtual std::string getTemplatePath() const;

    /// Why `source` (an endpoint `template-source` or cache `template-file`) may not be
    /// read or written through the management APIs, or "" when it may: it must resolve -
    /// symlinks included - to a path strictly inside the templates directory. An absolute
    /// or `../` source let a token holder overwrite any file the server can write (#189).
    std::string TemplatePathProblem(const std::string& source) const;
    // Non-empty when template.path is remote: the config service cannot create, change, delete or
    // reload endpoint files there (it would write under a mangled local path and claim success).
    std::string RemoteTemplateStorageProblem() const;
    std::string getCacheSchema() const;
    const std::unordered_map<std::string, ConnectionConfig>& getConnections() const;
    const RateLimitConfig& getRateLimitConfig() const;
    const DuckDBConfig& getDuckDBConfig() const;
    const HttpsConfig& getHttpsConfig() const;
    const CorsConfig& getCorsConfig() const { return cors_config; }
    bool isHttpsEnforced() const;
    bool isAuthEnabled() const;
    std::optional<OIDCConfig> getGlobalOIDCConfig() const;
    // A found endpoint plus the snapshot that keeps it alive.
    //
    // getEndpointForPath* used to hand out a raw pointer into a vector that
    // addEndpoint / removeEndpointByPath mutate on other threads, so the pointer
    // could dangle at any moment. Returning the snapshot alongside the pointer
    // makes the lifetime automatic: hold the EndpointRef for as long as you use
    // the endpoint.
    //
    // Bind with `auto ep = ...`. Note that `const auto* ep = ...` does not
    // compile - there is no implicit pointer conversion - so the real hazard is
    // calling .get() on a temporary and letting the pointer outlive the pin.
    // That overload is deleted below, which makes it a compile error too.
    class [[nodiscard]] EndpointRef {
    public:
        EndpointRef() = default;
        EndpointRef(std::shared_ptr<const std::vector<EndpointConfig>> snapshot,
                    const EndpointConfig* endpoint)
            : snapshot_(std::move(snapshot)), endpoint_(endpoint) {}

        const EndpointConfig* get() const & { return endpoint_; }
        // Deleted on rvalues: `cm.getEndpointForPath(p).get()` would return a
        // pointer into a snapshot released at the end of the full expression -
        // exactly the use-after-free the snapshot exists to prevent. Bind the ref
        // to a named variable first.
        const EndpointConfig* get() const && = delete;
        const EndpointConfig* operator->() const { return endpoint_; }
        const EndpointConfig& operator*() const { return *endpoint_; }
        explicit operator bool() const { return endpoint_ != nullptr; }

        friend bool operator==(const EndpointRef& r, std::nullptr_t) { return r.endpoint_ == nullptr; }
        friend bool operator!=(const EndpointRef& r, std::nullptr_t) { return r.endpoint_ != nullptr; }
        friend bool operator==(std::nullptr_t, const EndpointRef& r) { return r.endpoint_ == nullptr; }
        friend bool operator!=(std::nullptr_t, const EndpointRef& r) { return r.endpoint_ != nullptr; }

    private:
        std::shared_ptr<const std::vector<EndpointConfig>> snapshot_;
        const EndpointConfig* endpoint_ = nullptr;
    };

    EndpointRef getEndpointForPath(const std::string& path) const;
    EndpointRef getEndpointForPathAndMethod(const std::string& path, const std::string& httpMethod) const;
    /// getEndpointForPathAndMethod for the request in flight on this thread:
    /// the first call resolves and records the answer in its RequestContext,
    /// later calls for the same path and method reuse it (#243). Outside a
    /// request, or for another path, it is a plain lookup.
    EndpointRef getEndpointForRequest(const std::string& path, const std::string& httpMethod) const;
    // Returns a PINNED snapshot, not a reference.
    //
    // This used to return `const std::vector<EndpointConfig>&` into the live
    // snapshot and release the lock on the way out. Under copy-on-write that is
    // strictly worse than the raw-vector bug it replaced: a concurrent swap can
    // drop the last reference and DESTROY the vector the caller is iterating,
    // rather than merely reallocating its buffer. Returning the shared_ptr makes
    // the caller an owner for as long as it holds it.
    std::shared_ptr<const std::vector<EndpointConfig>> getEndpoints() const;

    // --- Endpoint snapshots (copy-on-write) -------------------------------
    //
    // The endpoint table is mutated at runtime by addEndpoint /
    // removeEndpointByPath, reachable from the config service and the MCP config
    // tools on request threads. Any raw pointer into it - which is what
    // getEndpointForPath* returns - dangles the moment a push_back reallocates.
    //
    // Pin a snapshot for as long as you intend to use pointers derived from it:
    //
    //     auto snap = cm.endpointsSnapshot();
    //     const EndpointConfig* ep = ConfigManager::findEndpoint(*snap, path, method);
    //     // `ep` stays valid while `snap` is alive, whatever other threads do.
    //
    // Mutations copy the vector, modify the copy, and swap the pointer, so a
    // snapshot already handed out is never written to.
    std::shared_ptr<const std::vector<EndpointConfig>> endpointsSnapshot() const;
    static const EndpointConfig* findEndpoint(const std::vector<EndpointConfig>& endpoints,
                                              const std::string& path);
    static const EndpointConfig* findEndpoint(const std::vector<EndpointConfig>& endpoints,
                                              const std::string& path,
                                              const std::string& httpMethod);
    const TemplateConfig& getTemplateConfig() const;

    // Log verbosity and shape. Precedence is CLI > environment > config > default,
    // resolved in main.cpp - an operator who passed --log-level meant it.
    const TracingConfig& getTracingConfig() const { return tracing_config; }

    // Built once from the tracing config and the EXISTING audit.redact (stored as AuditConfig::redact_keys), so
    // an operator configures redaction in one place rather than two lists that
    // silently diverge.
    const CapturePolicy& getCapturePolicy() const;
    const std::string& getLogLevel() const { return log_level; }
    int getStallTimeoutSeconds() const { return stall_timeout_s; }
    const std::string& getLogFormat() const { return log_format; }
    // `log-http-requests`, after FLAPI_LOG_HTTP_REQUESTS (which wins when set).
    bool getLogHttpRequests() const { return log_http_requests; }
    std::string getBasePath() const;
    std::string getDuckDBPath() const;
    ExtendedYamlParser& getYamlParser() { return yaml_parser; }
    std::filesystem::path getFullTemplatePath() const;

    // Get the file provider for VFS operations (local or remote file access)
    std::shared_ptr<IFileProvider> getFileProvider() const;

    const GlobalHeartbeatConfig& getGlobalHeartbeatConfig() const { return global_heartbeat_config; }
    const DuckLakeConfig& getDuckLakeConfig() const { return ducklake_config; }
    const DuckDBHardeningConfig& getDuckDBHardeningConfig() const { return duckdb_hardening_config; }
    // Whether the main configuration file was loaded from s3://, gs://, az://, http(s)://...
    bool isRemoteConfig() const;
    // Set from --cache-warmup / FLAPI_CACHE_WARMUP; wins over the config files.
    void setCacheWarmupOverride(std::optional<CacheWarmupMode> mode) { cache_warmup_override = mode; }
    // read-only > override > endpoint cache.warmup > ducklake.warmup
    CacheWarmupMode getEffectiveCacheWarmupMode(const EndpointConfig& endpoint) const;
    const MCPConfig& getMCPConfig() const { return mcp_config; }
    const StorageConfig& getStorageConfig() const { return storage_config; }
    const AuditConfig& getAuditConfig() const { return audit_config; }

    // Process-wide audit sink. Initialised lazily on first access from the
    // current AuditConfig; shared across REST and MCP handlers so every
    // request lands in the same JSONL stream.
    std::shared_ptr<AuditLogger> getAuditLogger() const;
    bool isTelemetryEnabled() const { return telemetry_enabled; }
    double getTelemetrySampleRate() const { return telemetry_sample_rate; }
    const AuthConfig& getGlobalAuthConfig() const { return global_auth_config; }
    // `jwt` -> `bearer`; every other value unchanged.
    static std::string normalizeAuthType(const std::string& type);
    // Copy the top-level `auth:` block onto an endpoint that declares none
    // (auth.declared == false), when global auth is enabled. No-op otherwise.
    void applyGlobalAuthDefault(EndpointConfig& endpoint) const;
    // An enabled endpoint `auth:` block that names no scheme and no credentials
    // (typically `roles:` only) takes the top-level block's type and
    // credentials, keeping its own roles when it lists any. Without an enabled
    // top-level block, or with credentials but no `type`, it throws
    // AUTH_NO_TYPE: such an endpoint could only ever answer 401.
    void inheritGlobalCredentials(AuthConfig& auth, bool declares_credentials, bool declares_roles,
                                  const std::string& location) const;
    // Load-time warning naming keys the chosen auth type never reads
    // (`users:` under bearer, `jwt-secret:` under basic, ...).
    static void warnIgnoredAuthKeys(const YAML::Node& auth_node, const std::string& type,
                                    const std::string& location);

    // Load MCP server instructions (inline or from file)
    std::string loadMCPInstructions() const;

    void refreshConfig();
    void addEndpoint(const EndpointConfig& endpoint);
    /// "" or why `candidate` may not be added: another loaded endpoint (from a
    /// different YAML file) already serves the same url-path and method. Both
    /// files are named.
    ///
    /// Also refuses a route that overlaps another endpoint's route for the
    /// same method (both can match one request path) when the more specific
    /// when their effective auth differs in any way - enabled, scheme, roles,
    /// or the identity source (users and their passwords, jwt-secret,
    /// jwt-issuer, OIDC issuer/audience/client, AWS secret) - unless the more
    /// specific endpoint sets `allow-shadowing: true`. Secrets are compared as
    /// digests and never appear in the message.
    ///
    /// `replacing`: the endpoint an update replaces, which is not compared.
    std::string DuplicateEndpointProblem(const EndpointConfig& candidate,
                                         const EndpointConfig* replacing = nullptr) const;
    /// Logs every allowed overlap of `candidate` with a loaded endpoint: a
    /// WARNING when their auth differs (naming both and which one answers the
    /// overlapping requests), INFO when it is the same.
    void LogRouteOverlaps(const EndpointConfig& candidate, const EndpointConfig* replacing = nullptr) const;
    /// The HTTP method as flapi stores and compares it: trimmed, upper-case,
    /// empty -> GET. Does not judge it; see HttpMethodProblem.
    static std::string CanonicalHttpMethod(const std::string& method);
    /// "" or why `method` (as written) is not one of GET, POST, PUT, PATCH,
    /// DELETE. `endpoint` names the endpoint in the message.
    static std::string HttpMethodProblem(const std::string& method, const std::string& endpoint);
    /// The route a url-path compiles to, parameter names discarded:
    /// `/customers/:id` and `/customers/:cid` have the same shape.
    static std::string RouteShape(const std::string& url_path);
    bool removeEndpointByPath(const std::string& path);
    bool replaceEndpoint(const EndpointConfig& endpoint);

    /// `listener` runs after every change of the loaded endpoint table (create,
    /// update, delete, reload, reload-file, the MCP config tools), on the thread
    /// that made it and outside the table's lock. The cache manager uses it to
    /// keep its readiness registry in step with the cache tables endpoints name.
    void addEndpointsChangedListener(std::function<void()> listener);
    
    std::unordered_map<std::string, std::string> getPropertiesForTemplates(const std::string& connectionName) const;

    crow::json::wvalue getFlapiConfig() const;
    crow::json::wvalue getEndpointsConfig() const;
    crow::json::wvalue serializeEndpointConfig(const EndpointConfig& config, EndpointJsonStyle style) const;
    /// With `merge_existing` the JSON is applied ON TOP of the endpoint that already has
    /// this url-path and method: whatever the JSON does not carry (auth users, rate-limit,
    /// per-field validators/defaults, ...) is kept, so GET -> PUT cannot strip an endpoint
    /// of its protections (#191). Without it (create) the endpoint starts empty.
    EndpointConfig deserializeEndpointConfig(const crow::json::rvalue& json, bool merge_existing = false) const;
    
    // YAML serialization/deserialization (for export, debugging)
    std::string serializeEndpointConfigToYaml(const EndpointConfig& config) const;
    EndpointConfig deserializeEndpointConfigFromYaml(const std::string& yaml_content) const;
    
    // Validation (does not modify files - preserves comments, formatting)
    struct ValidationResult {
        bool valid;
        std::vector<std::string> errors;
        std::vector<std::string> warnings;
    };
    ValidationResult validateEndpointConfig(const EndpointConfig& config) const;
    ValidationResult validateEndpointConfigFromYaml(const std::string& yaml_content) const;
    // Top-level keys of the main configuration that flAPI reads (or deliberately allows).
    static const std::vector<std::string>& KnownTopLevelKeys();

    ValidationResult validateEndpointConfigFile(const std::filesystem::path& file_path) const;
    
    // File persistence (only for programmatic creation/export - destroys formatting)
    void persistEndpointConfigToFile(const EndpointConfig& config, const std::filesystem::path& file_path) const;
    
    // Reload endpoint from disk (after external edit). `slug_or_path` may also
    // be an EndpointKey() ("POST /orders"), which names one of several
    // endpoints sharing a url-path.
    bool reloadEndpointConfig(const std::string& slug_or_path);

    /// "METHOD /path" for a REST endpoint, the MCP name otherwise: unique per
    /// loaded endpoint, unlike the url-path (GET /orders and POST /orders).
    static std::string EndpointKey(const EndpointConfig& endpoint);
    /// Look an endpoint up by EndpointKey(), falling back to a url-path / MCP
    /// name (the first match, as getEndpointForPath does).
    EndpointRef getEndpointForKey(const std::string& key) const;

    /// Reload every endpoint declared in `file`, or LOAD it when no loaded
    /// endpoint comes from it (a hand-written YAML added after startup).
    struct FileReloadResult {
        bool ok = false;
        int status = 200;              // 200 reloaded, 201 newly loaded, 4xx on error
        std::string error;
        bool loaded_new = false;
        std::vector<std::string> endpoints;   // EndpointKey() of each published endpoint
        std::vector<std::string> removed;     // EndpointKey() of each endpoint unloaded (file deleted)
        std::vector<std::string> warnings;
    };
    FileReloadResult reloadEndpointFile(const std::filesystem::path& file);

    /// Remove exactly this endpoint (url-path AND method, or MCP name), not
    /// every endpoint whose path matches - removeEndpointByPath("/orders")
    /// removed GET and POST alike.
    bool removeEndpoint(const EndpointConfig& endpoint);

    void warnOnUnknownTopLevelKeys() const;
    // Warns about keys that are parsed but acted on by nothing (server-name,
    // storage: with a local template path, connections.*.log-queries /
    // log-parameters / allow).
    void warnOnInertKeys() const;
    void printConfig() const;
    static void printYamlNode(const YAML::Node& node, int indent = 0);

protected:
    std::filesystem::path config_file;
    bool collect_load_errors_ = false;
    // Set while the startup scan of template.path loads endpoint files: a template-source outside it is refused.
    bool enforce_template_containment_ = false;
    std::vector<std::string> load_errors_;
    YAML::Node config;
    std::string project_name;
    TracingConfig tracing_config;
    mutable std::unique_ptr<CapturePolicy> capture_policy_;
    std::string log_level = "info";
    // Readiness fails when a request has been in flight longer than this, in
    // seconds. 0 disables the check.
    //
    // Guards against a backend that stops answering without failing: a wedged
    // SQLite attachment leaves DuckDB, the HTTP server and every other
    // connection healthy, so a `SELECT 1` probe passes while the endpoint is
    // dead (#116). The default is deliberately high - flAPI's answer to a
    // genuinely long query is the MCP Tasks extension, so a SYNCHRONOUS request
    // still running after this long is pathological rather than merely slow.
    int stall_timeout_s = 60;
    std::string log_format = "text";
    bool log_http_requests = false;
    std::string project_description;
    std::string cache_schema = "flapi";
    std::string server_name;
    int http_port = 8080;
    std::string http_host = "0.0.0.0";
    std::unordered_map<std::string, ConnectionConfig> connections;
    RateLimitConfig rate_limit_config;
    bool auth_enabled;
    AuthConfig global_auth_config;
    // Copy-on-write; never mutated in place once published. Guarded by
    // endpoints_mutex for writers; readers take the shared_ptr under the same
    // mutex and then need no lock at all. See endpointsSnapshot().
    std::shared_ptr<const std::vector<EndpointConfig>> endpoints_snapshot{
        std::make_shared<const std::vector<EndpointConfig>>()};
    // std::mutex is not movable, but ConfigManager is (config_manager_test.cpp
    // move-assigns one). Moving must not carry lock state: a freshly constructed
    // mutex is the correct result, because moving an object other threads are
    // using is not supported in the first place. Satisfies BasicLockable, so
    // std::lock_guard works unchanged.
    struct MovableMutex {
        MovableMutex() = default;
        MovableMutex(MovableMutex&&) noexcept {}
        MovableMutex& operator=(MovableMutex&&) noexcept { return *this; }
        MovableMutex(const MovableMutex&) = delete;
        MovableMutex& operator=(const MovableMutex&) = delete;
        void lock() { m.lock(); }
        void unlock() { m.unlock(); }
        bool try_lock() { return m.try_lock(); }
    private:
        std::mutex m;
    };
    mutable MovableMutex endpoints_mutex;
    // The remote-template file cache must outlive one call, or its TTL saves nothing.
    mutable MovableMutex remote_provider_mutex;
    mutable std::shared_ptr<IFileProvider> remote_caching_provider_;

    // Copy the current table, let `mutate` modify the copy, then publish it.
    // Every writer must go through here; nothing mutates a published vector.
    template <typename Fn>
    auto mutateEndpoints(Fn&& mutate) -> decltype(mutate(std::declval<std::vector<EndpointConfig>&>())) {
        std::vector<std::function<void()>> listeners;
        auto result = [&]() {
            std::lock_guard<MovableMutex> guard(endpoints_mutex);
            auto next = std::make_shared<std::vector<EndpointConfig>>(*endpoints_snapshot);
            auto r = mutate(*next);
            endpoints_snapshot = std::shared_ptr<const std::vector<EndpointConfig>>(std::move(next));
            listeners = endpoints_changed_listeners;
            return r;
        }();
        notifyEndpointsChanged(listeners);
        return result;
    }

    template <typename Fn>
    void mutateEndpointsVoid(Fn&& mutate) {
        std::vector<std::function<void()>> listeners;
        {
            std::lock_guard<MovableMutex> guard(endpoints_mutex);
            auto next = std::make_shared<std::vector<EndpointConfig>>(*endpoints_snapshot);
            mutate(*next);
            endpoints_snapshot = std::shared_ptr<const std::vector<EndpointConfig>>(std::move(next));
            listeners = endpoints_changed_listeners;
        }
        notifyEndpointsChanged(listeners);
    }

    // Called (outside the lock) after every published change of the endpoint
    // table; see addEndpointsChangedListener().
    std::vector<std::function<void()>> endpoints_changed_listeners;
    static void notifyEndpointsChanged(const std::vector<std::function<void()>>& listeners);
    std::filesystem::path base_path;
    DuckDBConfig duckdb_config;
    TemplateConfig template_config;
    HttpsConfig https_config;
    CorsConfig cors_config;
    GlobalHeartbeatConfig global_heartbeat_config;
    DuckLakeConfig ducklake_config;
    DuckDBHardeningConfig duckdb_hardening_config;
    std::optional<CacheWarmupMode> cache_warmup_override;
    MCPConfig mcp_config;
    StorageConfig storage_config;
    AuditConfig audit_config;
    std::shared_ptr<AuditLogger> audit_logger_;   // built in parseAuditConfig()
    bool telemetry_enabled = true;
    double telemetry_sample_rate = 1.0;
    ExtendedYamlParser yaml_parser;

    // Extracted classes for delegation (Facade pattern)
    std::unique_ptr<ConfigLoader> config_loader;
    std::unique_ptr<EndpointRepository> endpoint_repository;


    std::string getFullCacheSourcePath(const EndpointConfig& endpoint) const;
    
    void parseMainConfig();
    
    void applyEnvironmentPolicy();
    void parseConnections();
    void parseRateLimitConfig();
    void parseAuthConfig();
    void parseDuckDBConfig();
    void parseHttpsConfig();
    void parseCorsConfig();
    void parseTemplateConfig();
    void parseDuckLakeConfig();
    void parseDuckDBHardeningConfig();
    void parseMCPConfig();
    void parseAuditConfig();
    void parseTracingConfig();
    void parseStorageConfig();
    void parseEndpointConfig(const std::filesystem::path& config_file);
    void parseEndpointRequestFields(const YAML::Node& endpoint_config, EndpointConfig& endpoint);
    void parseEndpointValidators(const YAML::Node& req, RequestFieldConfig& field);
    // Enforces the x-mcp-header (request[].mcp-header) constraints at load time:
    // valid token characters, case-insensitively unique, never a secret name.
    // Throws on violation so a bad annotation fails the config load.
    void validateMcpHeaderAnnotations(const EndpointConfig& endpoint) const;
    void parseEndpointConnection(const YAML::Node& endpoint_config, EndpointConfig& endpoint);
    void parseEndpointRateLimit(const YAML::Node& endpoint_config, EndpointConfig& endpoint);
    void parseEndpointAuth(const YAML::Node& endpoint_config, EndpointConfig& endpoint);
    OIDCConfig parseOIDCConfigNode(const YAML::Node& oidc_node, const std::string& log_prefix = "") const;
    

    void parseEndpointCache(const YAML::Node& endpoint_config, const std::filesystem::path& endpoint_dir, EndpointConfig& endpoint);
    void parseGlobalHeartbeatConfig();
    void parseEndpointHeartbeat(const YAML::Node& endpoint_config, EndpointConfig& endpoint);


    std::string makePathRelativeToBasePathIfNecessary(const std::string& value) const;

    void validateEndpointConfig(const YAML::Node& endpoint_config, const std::string& file_path);
    template<typename T>
    T getValueOrThrow(const YAML::Node& node, const std::string& key, const std::string& yamlPath) const;

    template<typename T>
    T safeGet(const YAML::Node& node, const std::string& key, const std::string& path, const T& defaultValue) const;

    template<typename T>
    T safeGet(const YAML::Node& node, const std::string& key, const std::string& path) const;

public:
    static std::string secretNameToSecretId(const std::string& secret_name);
    // `auth.from-aws-secretmanager`, of an endpoint or of the top-level `auth:`.
    AuthFromSecretManagerConfig parseAwsSecretManagerNode(const YAML::Node& aws_node) const;

};

class ConfigurationError : public std::runtime_error {
public:
    ConfigurationError(const std::string& message, const std::string& yamlPath = "")
        : std::runtime_error(formatMessage(message, yamlPath)) {}

private:
    static std::string formatMessage(const std::string& message, const std::string& yamlPath) {
        std::ostringstream oss;
        oss << "Configuration error";
        if (!yamlPath.empty()) {
            oss << " at " << yamlPath;
        }
        oss << ": " << message;
        return oss.str();
    }
};

} // namespace flapi
