#pragma once

#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>
#include <yaml-cpp/yaml.h>
#include <nlohmann/json.hpp>
#include <filesystem>

#include "route_translator.hpp"

namespace flapi {

struct ConnectionConfig {
    std::string init;
    std::unordered_map<std::string, std::string> properties;
    bool log_queries;
    bool log_parameters;
    std::string allow;

    const std::string& getInit() const { return init; }
    void setInit(const std::string& initSql) { init = initSql; }
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

struct AuthConfig {
    bool enabled;
    std::string type;
    std::vector<AuthUser> users;
    std::string jwt_secret;
    std::string jwt_issuer;
};


struct RequestFieldConfig {
    std::string fieldName;
    std::string fieldIn;
    std::string description;
    bool required = false;  // Default to false
    std::vector<std::string> validators;
};

struct CacheConfig {
    std::string cacheTableName;
    std::string cacheSource;
    std::string refreshTime;
    bool refreshEndpoint = false;

    std::chrono::seconds getRefreshTimeInSeconds() const;
};

struct EndpointConfig {
    std::string urlPath;
    std::string method;
    std::vector<RequestFieldConfig> requestFields;
    std::string templateSource;
    std::vector<std::string> connection;
    RateLimitConfig rate_limit;
    AuthConfig auth;
    CacheConfig cache;
};

struct DuckDBConfig {
    std::unordered_map<std::string, std::string> settings;
    std::string db_path;  // New field for database path
};

class ConfigManager {
public:
    ConfigManager();
    void loadConfig(const std::string& config_file);
    void loadEndpointConfig(const std::string& config_file);

    // Getters for configuration values
    std::string getProjectName() const;
    std::string getProjectDescription() const;
    std::string getTemplatePath() const;
    std::string getCacheSchema() const;
    const std::unordered_map<std::string, ConnectionConfig>& getConnections() const;
    const RateLimitConfig& getRateLimitConfig() const;
    bool isHttpsEnforced() const;
    bool isAuthEnabled() const;
    const std::vector<EndpointConfig>& getEndpoints() const;
    std::string getBasePath() const;

    nlohmann::json getFlapiConfig() const;
    nlohmann::json getEndpointsConfig() const;
    void refreshConfig();

    const EndpointConfig* getEndpointForPath(const std::string& path) const;

    const DuckDBConfig& getDuckDBConfig() const;
    std::string getDuckDBPath() const;

    std::unordered_map<std::string, std::string> getPropertiesForTemplates(const std::string& connectionName) const;

private:
    YAML::Node config;
    std::string project_name;
    std::string project_description;
    std::string template_path;
    std::string cache_schema = "flapi_cache";
    std::unordered_map<std::string, ConnectionConfig> connections;
    RateLimitConfig rate_limit_config;
    bool enforce_https;
    bool auth_enabled;
    std::vector<EndpointConfig> endpoints;
    std::filesystem::path base_path;
    DuckDBConfig duckdb_config;
    
    void parseConfig();
    void parseEndpoints();
    void loadFlapiConfig();
    void loadEndpointConfigs();
    void parseDuckDBConfig();

    std::string makePathRelativeToBasePathIfNecessary(const std::string& value) const;
};

} // namespace flapi