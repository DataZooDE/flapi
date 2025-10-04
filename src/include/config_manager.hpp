#pragma once

#include <crow.h>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include <yaml-cpp/yaml.h>

#include "route_translator.hpp"
#include "extended_yaml_parser.hpp"
#include "path_utils.hpp"

namespace flapi {

struct TimeInterval {
    static std::optional<std::chrono::seconds> parseInterval(const std::string& interval);
};

struct ConnectionConfig {
    std::string init;
    std::unordered_map<std::string, std::string> properties;
    bool log_queries;
    bool log_parameters;
    std::string allow;

    const std::string& getInit() const { return init; }
    void setInit(const std::string& initSql) { init = initSql; }
};

struct HeartbeatConfig {
    bool enabled = false;
    std::unordered_map<std::string, std::string> params;
};

struct RateLimitConfig {
    bool enabled;
    int max;
    int interval;
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
    std::string secret_table;
    std::string init;

    const std::string& getInit() const { return init; }
    void setInit(const std::string& initSql) { init = initSql; }
};

struct AuthConfig {
    bool enabled;
    std::string type;
    std::vector<AuthUser> users;
    std::optional<AuthFromSecretManagerConfig> from_aws_secretmanager;
    std::string jwt_secret;
    std::string jwt_issuer;
};

struct ValidatorConfig {
    std::string type;
    std::string regex;
    int min;
    int max;
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
};

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

    bool hasCursor() const { return cursor.has_value(); }
    bool hasPrimaryKey() const { return !primary_keys.empty(); }
    bool hasTemplate() const { return template_file.has_value(); }
    std::chrono::seconds getRefreshTimeInSeconds() const;
};

struct EndpointConfig {
    std::string urlPath;
    std::string method;
    bool request_fields_validation = false;
    std::vector<RequestFieldConfig> request_fields;
    std::string templateSource;
    std::vector<std::string> connection;
    bool with_pagination = true;
    RateLimitConfig rate_limit;
    AuthConfig auth;
    CacheConfig cache;
    HeartbeatConfig heartbeat;
    
    // Path to the YAML configuration file for this endpoint (for reloading)
    std::string config_file_path;

    // MCP-specific metadata (optional)
    struct MCPToolInfo {
        std::string name;
        std::string description;
        std::string result_mime_type = "application/json";
    };

    struct MCPResourceInfo {
        std::string name;
        std::string description;
        std::string mime_type = "application/json";
    };

    struct MCPPromptInfo {
        std::string name;
        std::string description;
        std::string template_content;
        std::vector<std::string> arguments;
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

struct MCPToolParameter {
    std::string name;
    std::string description;
    std::string type = "string"; // string, number, boolean, array, object
    bool required = false;
    std::string default_value;
    std::vector<std::string> allowed_values; // for enum-like parameters
    std::unordered_map<std::string, std::string> constraints; // min, max, pattern, etc.
};

struct MCPToolConfig {
    std::string tool_name;
    std::string description;
    std::string input_schema_path; // JSON schema for tool parameters
    std::vector<MCPToolParameter> parameters;
    std::string template_source;
    std::vector<std::string> connection;
    std::string result_format = "json"; // json, csv, table
    bool cache_enabled = false;
    std::string cache_key_template;
    RateLimitConfig rate_limit;
    AuthConfig auth;
    CacheConfig cache;
    HeartbeatConfig heartbeat;
};

struct MCPServerConfig {
    bool enabled = false;
    std::string server_name = "flapi-mcp-server";
    std::string server_version = "0.1.0";
    std::string protocol_version = "2024-11-05";
    std::vector<std::string> capabilities = {"tools", "resources", "prompts", "sampling"};
    bool stdio_transport = false; // Use HTTP transport by default
    int mcp_port = 8081; // Different port from REST API
    std::string mcp_base_path = "/mcp";
};

struct DuckDBConfig {
    std::unordered_map<std::string, std::string> settings;
    std::string db_path;  // New field for database path
    std::vector<std::string> default_extensions;
};

struct TemplateConfig {
    std::string path;
    std::vector<std::string> environment_whitelist;

    bool isEnvironmentVariableAllowed(const std::string& varName) const {
        if (environment_whitelist.empty()) {
            return false;  // If no whitelist is specified, allow all variables
        }
        for (const auto& pattern : environment_whitelist) {
            std::regex regex(pattern);
            if (std::regex_match(varName, regex)) {
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

struct GlobalHeartbeatConfig {
    bool enabled = false;
    std::chrono::seconds workerInterval = std::chrono::seconds(60);
};

struct DuckLakeRetentionConfig {
    std::optional<std::size_t> keep_last_snapshots;
    std::optional<std::string> max_snapshot_age;
};

struct DuckLakeCompactionConfig {
    bool enabled = false;
    std::optional<std::string> schedule;
};

struct DuckLakeSchedulerConfig {
    bool enabled = false;
    std::optional<std::string> scan_interval;
};

struct DuckLakeConfig {
    bool enabled = false;
    std::string alias = "cache";
    std::string metadata_path;
    std::string data_path;
    DuckLakeRetentionConfig retention;
    DuckLakeCompactionConfig compaction;
    DuckLakeSchedulerConfig scheduler;
    std::optional<std::size_t> data_inlining_row_limit;
};

// Forward declaration
class EndpointConfigParser;

class ConfigManager {
    // Allow EndpointConfigParser to access protected parsing methods
    friend class EndpointConfigParser;
    
public:
    explicit ConfigManager(const std::filesystem::path& config_file);
    
    // Add virtual destructor
    virtual ~ConfigManager() = default;
    
    void loadConfig();
    void loadEndpointConfigsRecursively(const std::filesystem::path& dir);
    void loadEndpointConfig(const std::string& config_file);
    
    // Getters for configuration values
    std::string getProjectName() const;
    std::string getProjectDescription() const;
    std::string getServerName() const;
    int getHttpPort() const;
    void setHttpPort(int port);
    virtual std::string getTemplatePath() const;
    std::string getCacheSchema() const;
    const std::unordered_map<std::string, ConnectionConfig>& getConnections() const;
    const RateLimitConfig& getRateLimitConfig() const;
    const DuckDBConfig& getDuckDBConfig() const;
    const HttpsConfig& getHttpsConfig() const;
    bool isHttpsEnforced() const;
    bool isAuthEnabled() const;
    const EndpointConfig* getEndpointForPath(const std::string& path) const;
    const std::vector<EndpointConfig>& getEndpoints() const;
    const TemplateConfig& getTemplateConfig() const;
    std::string getBasePath() const;
    std::string getDuckDBPath() const;
    ExtendedYamlParser& getYamlParser() { return yaml_parser; }
    std::filesystem::path getFullTemplatePath() const;

    const GlobalHeartbeatConfig& getGlobalHeartbeatConfig() const { return global_heartbeat_config; }
    const DuckLakeConfig& getDuckLakeConfig() const { return ducklake_config; }


    void refreshConfig();
    void addEndpoint(const EndpointConfig& endpoint);
    bool removeEndpointByPath(const std::string& path);
    bool replaceEndpoint(const EndpointConfig& endpoint);
    
    std::unordered_map<std::string, std::string> getPropertiesForTemplates(const std::string& connectionName) const;

    crow::json::wvalue getFlapiConfig() const;
    crow::json::wvalue getEndpointsConfig() const;
    crow::json::wvalue serializeEndpointConfig(const EndpointConfig& config, EndpointJsonStyle style) const;
    EndpointConfig deserializeEndpointConfig(const crow::json::rvalue& json) const;
    
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
    ValidationResult validateEndpointConfigFile(const std::filesystem::path& file_path) const;
    
    // File persistence (only for programmatic creation/export - destroys formatting)
    void persistEndpointConfigToFile(const EndpointConfig& config, const std::filesystem::path& file_path) const;
    
    // Reload endpoint from disk (after external edit)
    bool reloadEndpointConfig(const std::string& slug_or_path);

    void printConfig() const;
    static void printYamlNode(const YAML::Node& node, int indent = 0);

protected:
    std::filesystem::path config_file;
    YAML::Node config;
    std::string project_name;
    std::string project_description;
    std::string cache_schema = "flapi";
    std::string server_name;
    int http_port = 8080;
    std::unordered_map<std::string, ConnectionConfig> connections;
    RateLimitConfig rate_limit_config;
    bool auth_enabled;
    std::vector<EndpointConfig> endpoints;
    std::filesystem::path base_path;
    DuckDBConfig duckdb_config;
    TemplateConfig template_config;
    HttpsConfig https_config;
    GlobalHeartbeatConfig global_heartbeat_config;
    DuckLakeConfig ducklake_config;
    ExtendedYamlParser yaml_parser;

    void parseConfig();

    std::string getFullCacheSourcePath(const EndpointConfig& endpoint) const;
    
    void parseMainConfig();
    void parseConnections();
    void parseRateLimitConfig();
    void parseAuthConfig();
    void parseDuckDBConfig();
    void parseHttpsConfig();
    void parseTemplateConfig();
    void parseDuckLakeConfig();
    void parseEndpointConfig(const std::filesystem::path& config_file);
    void parseEndpointRequestFields(const YAML::Node& endpoint_config, EndpointConfig& endpoint);
    void parseEndpointValidators(const YAML::Node& req, RequestFieldConfig& field);
    void parseEndpointConnection(const YAML::Node& endpoint_config, EndpointConfig& endpoint);
    void parseEndpointRateLimit(const YAML::Node& endpoint_config, EndpointConfig& endpoint);
    void parseEndpointAuth(const YAML::Node& endpoint_config, EndpointConfig& endpoint);
    

    void parseEndpointCache(const YAML::Node& endpoint_config, const std::filesystem::path& endpoint_dir, EndpointConfig& endpoint);
    void parseGlobalHeartbeatConfig();
    void parseEndpointHeartbeat(const YAML::Node& endpoint_config, EndpointConfig& endpoint);


    std::string makePathRelativeToBasePathIfNecessary(const std::string& value) const;

    void validateConfig();
    void validateEndpointConfig(const YAML::Node& endpoint_config, const std::string& file_path);
    template<typename T>
    T getValueOrThrow(const YAML::Node& node, const std::string& key, const std::string& yamlPath) const;

    template<typename T>
    T safeGet(const YAML::Node& node, const std::string& key, const std::string& path, const T& defaultValue) const;

    template<typename T>
    T safeGet(const YAML::Node& node, const std::string& key, const std::string& path) const;

public:
    static std::string secretNameToTableName(const std::string& secret_name);
    static std::string secretNameToSecretId(const std::string& secret_name);
    static std::string createDefaultAuthInit(const std::string& secret_name,
                                             const std::string& region,
                                             const std::string& secret_id,
                                             const std::string& secret_key);

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
