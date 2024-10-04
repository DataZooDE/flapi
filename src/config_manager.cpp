#include "config_manager.hpp"
#include <stdexcept>
#include <filesystem>
#include <yaml-cpp/yaml.h>
#include <fstream>
#include <iostream>
#include <crow.h> // Add this line

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
    try {
        config = YAML::LoadFile(config_file);
        base_path = std::filesystem::absolute(std::filesystem::path(config_file)).parent_path();
        
        validateConfig();
        parseConfig();

        // Load endpoint configurations
        CROW_LOG_INFO << "Loading endpoint configurations from: " << getTemplatePath();
        
        if (!std::filesystem::exists(getTemplatePath())) {
            throw ConfigurationError("Template path does not exist: " + getTemplatePath());
        }
        
        for (const auto& entry : std::filesystem::directory_iterator(getTemplatePath())) {
            if (entry.path().extension() == ".yaml") {
                loadEndpointConfig(entry.path().string());
            }
        }
    } catch (const YAML::Exception& e) {
        throw ConfigurationError(e.what(), config_file);
    }
}

void ConfigManager::loadEndpointConfig(const std::string& config_file) {
    try {
        YAML::Node endpoint_config = YAML::LoadFile(config_file);
        validateEndpointConfig(endpoint_config, config_file);
        
        std::string url_path = endpoint_config["urlPath"].as<std::string>();
        
        CROW_LOG_DEBUG << "Loading endpoint config from: " << config_file;
        CROW_LOG_DEBUG << "URL path: " << url_path;

        EndpointConfig endpoint;
        endpoint.urlPath = url_path;
        endpoint.templateSource = endpoint_config["templateSource"].as<std::string>();
        
        if (endpoint_config["request"]) {
            for (const auto& req : endpoint_config["request"]) {
                RequestFieldConfig field;
                field.fieldName = req["fieldName"].as<std::string>();
                field.fieldIn = req["fieldIn"].as<std::string>();
                field.description = req["description"].as<std::string>("");
                field.required = req["required"].as<bool>(false);

                if (req["validators"]) {
                    for (const auto& validator : req["validators"]) {
                        ValidatorConfig validatorConfig;
                        validatorConfig.type = validator["type"].as<std::string>();

                        if (validatorConfig.type == "string") {
                            validatorConfig.regex = validator["regex"].as<std::string>("");
                        } else if (validatorConfig.type == "int") {
                            validatorConfig.min = validator["min"].as<int>(std::numeric_limits<int>::min());
                            validatorConfig.max = validator["max"].as<int>(std::numeric_limits<int>::max());
                        } else if (validatorConfig.type == "date") {
                            validatorConfig.minDate = validator["min"].as<std::string>("");
                            validatorConfig.maxDate = validator["max"].as<std::string>("");
                        } else if (validatorConfig.type == "time") {
                            validatorConfig.minTime = validator["min"].as<std::string>("");
                            validatorConfig.maxTime = validator["max"].as<std::string>("");
                        } else if (validatorConfig.type == "enum") {
                            validatorConfig.allowedValues = validator["allowedValues"].as<std::vector<std::string>>();
                        }

                        // Parse SQL injection prevention setting
                        validatorConfig.preventSqlInjection = validator["preventSqlInjection"].as<bool>(true);

                        field.validators.push_back(validatorConfig);
                    }
                }

                endpoint.requestFields.push_back(field);
                CROW_LOG_DEBUG << "\tAdded request field: " << field.fieldName << " (" << field.fieldIn << ")";
            }
        }
        
        if (endpoint_config["connection"]) {
            endpoint.connection = endpoint_config["connection"].as<std::vector<std::string>>();
        }
        
        // Parse rate limit configuration
        if (endpoint_config["rate-limit"]) {
            auto rate_limit = endpoint_config["rate-limit"];
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
                endpoint.cache.cacheSource = cache_config["cacheSource"].as<std::string>("");
                endpoint.cache.refreshTime = cache_config["refreshTime"].as<std::string>("1h");
                endpoint.cache.refreshEndpoint = cache_config["refreshEndpoint"].as<bool>(false);
                CROW_LOG_DEBUG << "\tAdded cache config: " << endpoint.cache.cacheTableName 
                               << " (refresh time: " << endpoint.cache.refreshTime << ")";
        }
        
        endpoints.push_back(endpoint);
        CROW_LOG_INFO << "Loaded endpoint config for: " << url_path;
    } catch (const YAML::Exception& e) {
        throw ConfigurationError(e.what(), config_file);
    }
}

void ConfigManager::validateConfig() {
    if (!config["name"]) {
        throw ConfigurationError("Missing 'name' field");
    }
    if (!config["template"] || !config["template"]["path"]) {
        throw ConfigurationError("Missing 'template.path' field");
    }
    if (!config["connections"]) {
        throw ConfigurationError("Missing 'connections' field");
    }
}

void ConfigManager::validateEndpointConfig(const YAML::Node& endpoint_config, const std::string& file_path) {
    if (!endpoint_config["urlPath"]) {
        throw ConfigurationError("Missing 'urlPath' field", file_path);
    }
    if (!endpoint_config["templateSource"]) {
        throw ConfigurationError("Missing 'templateSource' field", file_path);
    }
    // Add more validation as needed
}

template<typename T>
T ConfigManager::getValueOrThrow(const YAML::Node& node, const std::string& key, const std::string& yamlPath) const {
    if (!node[key]) {
        throw ConfigurationError("Missing '" + key + "' field", yamlPath);
    }
    try {
        return node[key].as<T>();
    } catch (const YAML::BadConversion& e) {
        throw ConfigurationError("Invalid type for '" + key + "' field", yamlPath);
    }
}

void ConfigManager::parseConfig() {
    project_name = getValueOrThrow<std::string>(config, "name", "flapi.yaml");
    project_description = getValueOrThrow<std::string>(config, "description", "flapi.yaml");
    
    server_name = config["server-name"].as<std::string>("localhost");
    enforce_https = config["enforce-https"]["enabled"].as<bool>(false);
    http_port = config["http-port"].as<int>(enforce_https ? 8443 : 8080);

    // Parse template configuration
    parseTemplateConfig();

    // Parse connections
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

    // Parse auth
    auth_enabled = config["auth"]["enabled"].as<bool>(false);

    // Parse DuckDB configuration
    parseDuckDBConfig();
}

void ConfigManager::parseTemplateConfig() {
    auto template_node = config["template"];
    if (template_node) {
        template_config.path = template_node["path"].as<std::string>();
        std::filesystem::path template_path_relative(template_config.path);
        template_config.path = std::filesystem::absolute((base_path / template_path_relative).lexically_normal()).string();

        if (template_node["environment-whitelist"]) {
            template_config.environment_whitelist = template_node["environment-whitelist"].as<std::vector<std::string>>();
        }
    } else {
        throw std::runtime_error("Template configuration is missing in flapi.yaml");
    }
}

const TemplateConfig& ConfigManager::getTemplateConfig() const {
    return template_config;
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

const DuckDBConfig& ConfigManager::getDuckDBConfig() const {
    return duckdb_config;
}

void ConfigManager::parseEndpoints() {
    std::filesystem::path endpoints_dir = std::filesystem::path(template_config.path);
    
    std::cout << "Parsing endpoints from: " << endpoints_dir << std::endl;
    
    if (!std::filesystem::exists(endpoints_dir)) {
        throw std::runtime_error("Endpoints directory does not exist: " + endpoints_dir.string());
    }
    
    for (const auto& entry : std::filesystem::directory_iterator(endpoints_dir)) {
        if (entry.path().extension() == ".yaml") {
            YAML::Node endpoint_config = YAML::LoadFile(entry.path().string());
            EndpointConfig endpoint;
            
            endpoint.urlPath = endpoint_config["urlPath"].as<std::string>();
            endpoint.method = endpoint_config["method"].as<std::string>("GET");
            
            if (endpoint_config["request"]) {
                for (const auto& req : endpoint_config["request"]) {
                    RequestFieldConfig field;
                    field.fieldName = req["fieldName"].as<std::string>();
                    field.fieldIn = req["fieldIn"].as<std::string>();
                    field.description = req["description"].as<std::string>("");
                    if (! req["validators"]) {
                        continue;
                    }

                    for (const auto& validator : req["validators"]) {
                        ValidatorConfig validatorConfig;
                        validatorConfig.type = validator["type"].as<std::string>();
                        if (validatorConfig.type == "string") {
                            validatorConfig.regex = validator["regex"].as<std::string>("");
                        } else if (validatorConfig.type == "int") {
                            validatorConfig.min = validator["min"].as<int>(std::numeric_limits<int>::min());
                            validatorConfig.max = validator["max"].as<int>(std::numeric_limits<int>::max());
                        }
                        field.validators.push_back(validatorConfig);
                    }
                    
                    endpoint.requestFields.push_back(field);
                }
            }
            
            endpoint.templateSource = endpoint_config["templateSource"].as<std::string>();
            
            if (endpoint_config["connection"]) {
                endpoint.connection = endpoint_config["connection"].as<std::vector<std::string>>();
            }

            // Parse rate limit configuration
            if (endpoint_config["rate-limit"]) {
                auto rate_limit_node = endpoint_config["rate-limit"];
                endpoint.rate_limit.enabled = rate_limit_node["enabled"].as<bool>(false);
                endpoint.rate_limit.interval = rate_limit_node["interval"].as<int>(60);
                endpoint.rate_limit.max = rate_limit_node["max"].as<int>(500);
            } else {
                endpoint.rate_limit.enabled = false;
            }
            
            endpoints.push_back(endpoint);
        }
    }
}

// Implement getter methods
std::string ConfigManager::getProjectName() const { return project_name; }
std::string ConfigManager::getProjectDescription() const { return project_description; }
std::string ConfigManager::getServerName() const { return server_name; }
int ConfigManager::getHttpPort() const { return http_port; }
void ConfigManager::setHttpPort(int port) { http_port = port; }
std::string ConfigManager::getTemplatePath() const { return template_config.path; }
std::string ConfigManager::getCacheSchema() const { return cache_schema; }
const std::unordered_map<std::string, ConnectionConfig>& ConfigManager::getConnections() const { return connections; }
const RateLimitConfig& ConfigManager::getRateLimitConfig() const { return rate_limit_config; }
bool ConfigManager::isHttpsEnforced() const { return enforce_https; }
bool ConfigManager::isAuthEnabled() const { return auth_enabled; }
const std::vector<EndpointConfig>& ConfigManager::getEndpoints() const { return endpoints; }
std::string ConfigManager::getBasePath() const { return base_path.string(); }

crow::json::wvalue ConfigManager::getFlapiConfig() const {
    crow::json::wvalue result;
    
    // Manually construct the JSON object from the YAML data
    result["name"] = project_name;
    result["description"] = project_description;
    result["template-path"] = template_config.path;
    
    // Add connections
    crow::json::wvalue connectionsJson;
    for (const auto& [name, conn] : connections) {
        crow::json::wvalue connJson;
        connJson["init"] = conn.init;
        connJson["log-queries"] = conn.log_queries;
        connJson["log-parameters"] = conn.log_parameters;
        connJson["allow"] = conn.allow;

        crow::json::wvalue propertiesJson;
        for (const auto& [key, value] : conn.properties) {
            propertiesJson[key] = value;
        }
        connJson["properties"] = std::move(propertiesJson);
        connectionsJson[name] = std::move(connJson);
    }
    result["connections"] = std::move(connectionsJson);
    
    // Add other configurations
    result["enforce-https"]["enabled"] = enforce_https;
    result["auth"]["enabled"] = auth_enabled;
    
    return result;
}

crow::json::wvalue ConfigManager::getEndpointsConfig() const {
    crow::json::wvalue endpointsJson;
    for (const auto& endpoint : endpoints) {
        crow::json::wvalue endpointJson;
        endpointJson["urlPath"] = endpoint.urlPath;
        endpointJson["templateSource"] = endpoint.templateSource;
        endpointJson["connection"] = endpoint.connection;

        std::vector<crow::json::wvalue> requestJson;
        for (const auto& req : endpoint.requestFields) {
            crow::json::wvalue fieldJson;
            fieldJson["fieldName"] = req.fieldName;
            fieldJson["fieldIn"] = req.fieldIn;
            fieldJson["description"] = req.description;

            std::vector<crow::json::wvalue> validatorsJson;
            for (const auto& validator : req.validators) {
                crow::json::wvalue validatorJson;
                validatorJson["type"] = validator.type;
                if (validator.type == "string") {
                    validatorJson["regex"] = validator.regex;
                } else if (validator.type == "int") {
                    validatorJson["min"] = validator.min;
                    validatorJson["max"] = validator.max;   
                }
                validatorsJson.push_back(std::move(validatorJson));
            }
            fieldJson["validators"] = std::move(validatorsJson);
            
            requestJson.push_back(std::move(fieldJson));
        }
        endpointJson["request"] = std::move(requestJson);

        endpointsJson[endpoint.urlPath] = std::move(endpointJson);
    }
    return endpointsJson;
}

void ConfigManager::refreshConfig() {
    loadFlapiConfig();
    loadEndpointConfigs();
}

void ConfigManager::loadFlapiConfig() {
    config = YAML::LoadFile(base_path / "flapi.yaml");
    parseConfig();
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

} // namespace flapi