#include "config_manager.hpp"
#include <stdexcept>
#include <filesystem>
#include <yaml-cpp/yaml.h>
#include <fstream>
#include <iostream>
#include <crow.h>

namespace flapi {

/**
 * @brief Get the refresh time in seconds, the refresh time is a string in the format of "1h" for 1 hour. 
 * Available units are "s" for seconds, "m" for minutes, "h" for hours, "d" for days.   
 * @return The refresh time in seconds
 */
std::chrono::seconds CacheConfig::getRefreshTimeInSeconds() const 
{
    if (refreshTime.empty()) {
        return std::chrono::seconds(0);
    }

    size_t value = 0;
    char unit = '\0';
    std::istringstream iss(refreshTime);
    iss >> value >> unit;

    if (iss.fail() || value < 0) {
        throw std::runtime_error("Invalid refresh time format: " + refreshTime);
    }

    switch (std::tolower(unit)) {
        case 's':
            return std::chrono::seconds(value);
        case 'm':
            return std::chrono::minutes(value);
        case 'h':
            return std::chrono::hours(value);
        case 'd':
            return std::chrono::hours(value * 24);
        default:
            throw std::runtime_error("Invalid time unit in refresh time: " + refreshTime);
    }
}

ConfigManager::ConfigManager() : enforce_https(false), auth_enabled(false) {}

void ConfigManager::loadConfig(const std::string& config_file) {
    config = YAML::LoadFile(config_file);
    base_path = std::filesystem::absolute(std::filesystem::path(config_file)).parent_path();
    parseMainConfig();

    std::filesystem::path template_path_full = getFullTemplatePath();
    CROW_LOG_INFO << "Loading endpoint configurations from: " << template_path_full.string();
    
    if (!std::filesystem::exists(template_path_full)) {
        throw std::runtime_error("Template path does not exist: " + template_path_full.string());
    }
    
    loadEndpointConfigsRecursively(template_path_full);
}

void ConfigManager::parseMainConfig() {
    project_name = config["name"].as<std::string>();
    project_description = config["description"].as<std::string>();
    template_path = getFullTemplatePath().string();

    parseConnections();
    parseRateLimitConfig();
    parseAuthConfig();
    parseDuckDBConfig();
}

void ConfigManager::parseConnections() {
    try {
        auto connections_node = config["connections"];
        for (const auto& conn : connections_node) {
            std::string conn_name = conn.first.as<std::string>();
            ConnectionConfig conn_config;
            conn_config.init = conn.second["init"].as<std::string>("");
            conn_config.log_queries = conn.second["log-queries"].as<bool>(false);
            conn_config.log_parameters = conn.second["log-parameters"].as<bool>(false);
            conn_config.allow = conn.second["allow"].as<std::string>("*");

            auto properties = conn.second["properties"];
            for (const auto& prop : properties) {
                conn_config.properties[prop.first.as<std::string>()] = prop.second.as<std::string>();
            }

            connections[conn_name] = conn_config;
        }
    } catch (const std::exception& e) {
        throw std::runtime_error("Error parsing connections: " + std::string(e.what()));
    }
}

void ConfigManager::parseRateLimitConfig() {
    try {
        if (config["rate-limit"]) {
            auto rate_limit_node = config["rate-limit"]["options"];
            rate_limit_config.interval = rate_limit_node["interval"].as<int>();
            rate_limit_config.max = rate_limit_node["max"].as<int>();
        }
    } catch (const std::exception& e) {
        throw std::runtime_error("Error parsing rate limit config: " + std::string(e.what()));
    }
}

void ConfigManager::parseAuthConfig() {
    enforce_https = config["enforce-https"]["enabled"].as<bool>(false);
    auth_enabled = config["auth"]["enabled"].as<bool>(false);
}

void ConfigManager::parseDuckDBConfig() {
    if (config["duckdb"]) {
        auto duckdb_node = config["duckdb"];
        for (const auto& setting : duckdb_node) {
            std::string key = setting.first.as<std::string>();
            std::string value = setting.second.as<std::string>();
            if (key == "db_path") {
                duckdb_config.db_path = value;
            } else {
                duckdb_config.settings[key] = value;
            }
        }
    }
}

void ConfigManager::loadEndpointConfig(const std::string& base_config_dir, const std::string& config_file) {
    YAML::Node endpoint_config = YAML::LoadFile(config_file);
    std::string url_path = endpoint_config["urlPath"].as<std::string>();
    
    CROW_LOG_DEBUG << "Loading endpoint config from: " << config_file;
    CROW_LOG_DEBUG << "URL path: " << url_path;

    EndpointConfig endpoint;
    endpoint.urlPath = url_path;
    
    // Make templateSource path relative to the endpoint config file
    std::filesystem::path config_dir = std::filesystem::path(config_file).parent_path();
    std::filesystem::path template_source_path = config_dir / endpoint_config["templateSource"].as<std::string>();
    endpoint.templateSource = std::filesystem::relative(template_source_path, base_config_dir).string();

    if (endpoint_config["request"]) {
        for (const auto& req : endpoint_config["request"]) {
            RequestFieldConfig field;
            field.fieldName = req["fieldName"].as<std::string>();
            field.fieldIn = req["fieldIn"].as<std::string>();
            field.description = req["description"].as<std::string>("");
            if (req["validators"]) {
                field.validators = req["validators"].as<std::vector<std::string>>();
            }
            endpoint.requestFields.push_back(field);
            CROW_LOG_DEBUG << "\tAdded request field: " << field.fieldName << " (" << field.fieldIn << ")";
        }
    }
    
    if (endpoint_config["connection"]) {
        endpoint.connection = endpoint_config["connection"].as<std::vector<std::string>>();
    }
    
    // Parse rate limit configuration
    if (endpoint_config["rateLimit"]) {
        auto rate_limit = endpoint_config["rateLimit"];
        endpoint.rate_limit.enabled = rate_limit["enabled"].as<bool>(false);
        endpoint.rate_limit.max = rate_limit["max"].as<int>(0);
        endpoint.rate_limit.interval = rate_limit["interval"].as<int>(0);
    } else {
        endpoint.rate_limit.enabled = false;
    }
    
    // Parse auth configuration
    if (endpoint_config["auth"]) {
        auto auth = endpoint_config["auth"];
        endpoint.auth.enabled = auth["enabled"].as<bool>(false);
        endpoint.auth.type = auth["type"].as<std::string>("basic");
        if (auth["users"]) {
            auto users = auth["users"];
            for (const auto& user : users) {
                AuthUser auth_user;
                auth_user.username = user["username"].as<std::string>();
                auth_user.password = user["password"].as<std::string>();
                auth_user.roles = user["roles"].as<std::vector<std::string>>();
                endpoint.auth.users.push_back(auth_user);
            }
        }
        endpoint.auth.jwt_secret = auth["jwt_secret"].as<std::string>("");
        endpoint.auth.jwt_issuer = auth["jwt_issuer"].as<std::string>("");
    } else {
        endpoint.auth.enabled = false;
    }
    // Parse cache configuration
    if (endpoint_config["cache"]) {
        auto cache_config = endpoint_config["cache"];
        
        CacheConfig cache;
        endpoint.cache.cacheTableName = cache_config["cacheTableName"].as<std::string>();
        
        // Make cacheSource path relative to the endpoint config file
        if (cache_config["cacheSource"]) {
            std::filesystem::path cache_source_path = config_dir / cache_config["cacheSource"].as<std::string>();
            endpoint.cache.cacheSource = std::filesystem::relative(cache_source_path, base_config_dir).string();
        }
        
        endpoint.cache.refreshTime = cache_config["refreshTime"].as<std::string>("1h");
        endpoint.cache.refreshEndpoint = cache_config["refreshEndpoint"].as<bool>(false);
        CROW_LOG_DEBUG << "\tAdded cache config: " << endpoint.cache.cacheTableName 
                       << " (refresh time: " << endpoint.cache.refreshTime << ")";
        CROW_LOG_DEBUG << "\tCache source: " << endpoint.cache.cacheSource;
    }
    
    endpoints.push_back(endpoint);
    CROW_LOG_INFO << "Loaded endpoint config for: " << url_path;
}

void ConfigManager::parseEndpoints() {
    std::filesystem::path endpoints_dir = std::filesystem::path(template_path);
    
    std::cout << "Parsing endpoints from: " << endpoints_dir << std::endl;
    
    if (!std::filesystem::exists(endpoints_dir)) {
        throw std::runtime_error("Endpoints directory does not exist: " + endpoints_dir.string());
    }
    
    for (const auto& entry : std::filesystem::directory_iterator(endpoints_dir)) {
        if (entry.path().extension() == ".yaml") {
            loadEndpointConfig(endpoints_dir.string(), entry.path().string());
        }
    }
}

// Implement getter methods
std::string ConfigManager::getProjectName() const { return project_name; }
std::string ConfigManager::getProjectDescription() const { return project_description; }
std::string ConfigManager::getTemplatePath() const { return template_path; }
std::string ConfigManager::getCacheSchema() const { return cache_schema; }
const std::unordered_map<std::string, ConnectionConfig>& ConfigManager::getConnections() const { return connections; }
const RateLimitConfig& ConfigManager::getRateLimitConfig() const { return rate_limit_config; }
bool ConfigManager::isHttpsEnforced() const { return enforce_https; }
bool ConfigManager::isAuthEnabled() const { return auth_enabled; }
const std::vector<EndpointConfig>& ConfigManager::getEndpoints() const { return endpoints; }
std::string ConfigManager::getBasePath() const { return base_path.string(); }

nlohmann::json ConfigManager::getFlapiConfig() const {
    nlohmann::json result;
    
    // Manually construct the JSON object from the YAML data
    result["name"] = project_name;
    result["description"] = project_description;
    result["template-path"] = template_path;
    
    // Add connections
    nlohmann::json connectionsJson;
    for (const auto& [name, conn] : connections) {
        nlohmann::json connJson;
        connJson["init"] = conn.init;
        connJson["log-queries"] = conn.log_queries;
        connJson["log-parameters"] = conn.log_parameters;
        connJson["allow"] = conn.allow;
        connJson["properties"] = conn.properties;
        connectionsJson[name] = connJson;
    }
    result["connections"] = connectionsJson;
    
    // Add rate limit config
    result["rate-limit"]["options"]["interval"] = rate_limit_config.interval;
    result["rate-limit"]["options"]["max"] = rate_limit_config.max;
    
    // Add other configurations
    result["enforce-https"]["enabled"] = enforce_https;
    result["auth"]["enabled"] = auth_enabled;
    
    
    return result;
}

nlohmann::json ConfigManager::getEndpointsConfig() const {
    nlohmann::json endpointsJson;
    for (const auto& endpoint : endpoints) {
        nlohmann::json endpointJson;
        endpointJson["urlPath"] = endpoint.urlPath;
        endpointJson["templateSource"] = endpoint.templateSource;
        endpointJson["connection"] = endpoint.connection;

        nlohmann::json requestJson;
        for (const auto& req : endpoint.requestFields) {
            nlohmann::json fieldJson;
            fieldJson["fieldName"] = req.fieldName;
            fieldJson["fieldIn"] = req.fieldIn;
            fieldJson["description"] = req.description;
            fieldJson["validators"] = req.validators;
            requestJson.push_back(fieldJson);
        }
        endpointJson["request"] = requestJson;

        endpointsJson[endpoint.urlPath] = endpointJson;
    }
    return endpointsJson;
}

void ConfigManager::refreshConfig() {
    loadFlapiConfig();
    loadEndpointConfigs();
}

void ConfigManager::loadFlapiConfig() {
    config = YAML::LoadFile(base_path / "flapi.yaml");
    parseMainConfig();
}

void ConfigManager::loadEndpointConfigs() {
    endpoints.clear();
    parseEndpoints();
}

// Add this method to the ConfigManager class implementation

const EndpointConfig* ConfigManager::getEndpointForPath(const std::string& path) const {
    for (const auto& endpoint : endpoints) {
        std::vector<std::string> paramNames;
        std::map<std::string, std::string> pathParams;
        
        if (RouteTranslator::matchAndExtractParams(endpoint.urlPath, path, paramNames, pathParams)) {
            return &endpoint;
        }
    }
    return nullptr;
}

std::string ConfigManager::getDuckDBPath() const {
    return duckdb_config.db_path.empty() ? ":memory:" : duckdb_config.db_path;
}

std::string ConfigManager::makePathRelativeToBasePathIfNecessary(const std::string& value) const {
        if (value.find("./") == 0 || value.find("../") == 0) {
            std::filesystem::path relativePath(value);
            std::filesystem::path absolutePath = std::filesystem::absolute(base_path / relativePath).lexically_normal();
            return absolutePath.string();
        }
        return value;
    }

std::unordered_map<std::string, std::string> ConfigManager::getPropertiesForTemplates(const std::string& connectionName) const {
    if (connections.find(connectionName) == connections.end()) {
        return {};
    }

    auto props = connections.at(connectionName).properties;
    std::unordered_map<std::string, std::string> propsForTemplates;

    for (const auto& [key, value] : props) {
        propsForTemplates[key] = makePathRelativeToBasePathIfNecessary(value); // Use the new method
    }

    return propsForTemplates;
}

void ConfigManager::loadEndpointConfigsRecursively(const std::filesystem::path& dir) {
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".yaml") {
            loadEndpointConfig(dir.string(), entry.path().string());
        }
    }
}

std::string ConfigManager::getFullCacheSourcePath(const EndpointConfig& endpoint) const {
    if (endpoint.cache.cacheSource.empty()) {
        return "";
    }
    return (base_path / endpoint.cache.cacheSource).string();
}

std::filesystem::path ConfigManager::getFullTemplatePath() const {
    std::filesystem::path template_path_relative = config["template-path"].as<std::string>();
    return std::filesystem::absolute((base_path / template_path_relative).lexically_normal());
}

const DuckDBConfig& ConfigManager::getDuckDBConfig() const {
    return duckdb_config;
}

} // namespace flapi