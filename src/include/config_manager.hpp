#pragma once

#include <crow.h>
#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>
#include <yaml-cpp/yaml.h>
#include <filesystem>
#include <regex>
#include <stdexcept>
#include <sstream>

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

class ConfigManager {
public:
    ConfigManager();
    void loadConfig(const std::string& config_file);
    void loadEndpointConfig(const std::string& config_file);

    // Getters for configuration values
    std::string getProjectName() const;
    std::string getProjectDescription() const;
    virtual std::string getTemplatePath() const;
    std::string getCacheSchema() const;
    const std::unordered_map<std::string, ConnectionConfig>& getConnections() const;
    const RateLimitConfig& getRateLimitConfig() const;
    bool isHttpsEnforced() const;
    bool isAuthEnabled() const;
    const std::vector<EndpointConfig>& getEndpoints() const;
    std::string getBasePath() const;

    crow::json::wvalue getFlapiConfig() const;
    crow::json::wvalue getEndpointsConfig() const;
    void refreshConfig();

    const EndpointConfig* getEndpointForPath(const std::string& path) const;

    const DuckDBConfig& getDuckDBConfig() const;
    std::string getDuckDBPath() const;

    std::unordered_map<std::string, std::string> getPropertiesForTemplates(const std::string& connectionName) const;

    const TemplateConfig& getTemplateConfig() const;

protected:
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
    TemplateConfig template_config;
    
    void parseConfig();
    void parseEndpoints();
    void loadFlapiConfig();
    void loadEndpointConfigs();
    void parseDuckDBConfig();
    void parseTemplateConfig();

    std::string makePathRelativeToBasePathIfNecessary(const std::string& value) const;

    // Add these new private methods
    void validateConfig();
    void validateEndpointConfig(const YAML::Node& endpoint_config, const std::string& file_path);
    template<typename T>
    T getValueOrThrow(const YAML::Node& node, const std::string& key, const std::string& yamlPath) const;
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