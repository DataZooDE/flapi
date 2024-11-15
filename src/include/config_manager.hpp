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
    std::vector<ValidatorConfig> validators;
};

struct CacheConfig {
    std::string cacheTableName;
    std::string cacheSource;
    std::string refreshTime;
    bool refreshEndpoint = false;
    std::size_t maxPreviousTables = 5;

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
    HeartbeatConfig heartbeat;
};

struct DuckDBConfig {
    std::unordered_map<std::string, std::string> settings;
    std::string db_path;  // New field for database path
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

class ConfigManager {
public:
    ConfigManager(const std::filesystem::path& config_file);
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
    std::filesystem::path getFullTemplatePath() const;

    const GlobalHeartbeatConfig& getGlobalHeartbeatConfig() const { return global_heartbeat_config; }

    void refreshConfig();
    void addEndpoint(const EndpointConfig& endpoint);
    
    std::unordered_map<std::string, std::string> getPropertiesForTemplates(const std::string& connectionName) const;

    crow::json::wvalue getFlapiConfig() const;
    crow::json::wvalue getEndpointsConfig() const;

    void printConfig() const;
    static void printYamlNode(const YAML::Node& node, int indent = 0);


protected:
    std::filesystem::path config_file;
    YAML::Node config;
    std::string project_name;
    std::string project_description;
    std::string cache_schema = "flapi_cache";
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

    void parseConfig();

    std::string getFullCacheSourcePath(const EndpointConfig& endpoint) const;
    
    void parseMainConfig();
    void parseConnections();
    void parseRateLimitConfig();
    void parseAuthConfig();
    void parseDuckDBConfig();
    void parseHttpsConfig();
    void parseTemplateConfig();
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
