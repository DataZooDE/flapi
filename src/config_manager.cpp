#include "config_manager.hpp"
#include <stdexcept>
#include <filesystem>
#include <yaml-cpp/yaml.h>
#include <fstream>
#include <iostream>
#include <crow.h> // Add this line
#include <iomanip>
#include <crow/logging.h>

namespace flapi {

/**
 * @brief Get the refresh time in seconds, the refresh time is a string in the format of "1h" for 1 hour. 
 * Available units are "s" for seconds, "m" for minutes, "h" for hours, "d" for days.   
 * @return The refresh time in seconds
 */
std::chrono::seconds CacheConfig::getRefreshTimeInSeconds() const {
    auto interval = TimeInterval::parseInterval(refreshTime);
    if (!interval) {
        throw std::runtime_error("Invalid refresh time format: " + refreshTime + 
                               ". Expected format: <number>[s|m|h|d] (e.g., 30s, 5m, 2h, 1d)");
    }
    return *interval;
}

ConfigManager::ConfigManager(const std::filesystem::path& config_file)
    : config_file(config_file), auth_enabled(false), yaml_parser()
{}

// Main configuration loading and parsing methods
void ConfigManager::loadConfig() {
    try {
        CROW_LOG_INFO << "Loading configuration file: " << config_file;

        // Use ExtendedYamlParser to load the main config file
        auto result = yaml_parser.parseFile(config_file);
        if (!result.success) {
            throw std::runtime_error("Failed to parse config file: " + result.error_message);
        }
        config = result.node;

        parseMainConfig();

        std::filesystem::path template_path = getTemplateConfig().path;
        loadEndpointConfigsRecursively(template_path);
        CROW_LOG_INFO << "Configuration loaded successfully";
    } catch (const YAML::Exception& e) {
        std::ostringstream error_msg;
        error_msg << "Error loading configuration file: " << config_file << ", Error: " << e.what();
        CROW_LOG_ERROR << error_msg.str();
        CROW_LOG_DEBUG << "Current configuration structure:";
        printConfig();
        throw std::runtime_error(error_msg.str());
    }
}

void ConfigManager::parseMainConfig() {
    try {
        CROW_LOG_INFO << "Parsing main configuration";
        base_path = config_file.parent_path();

        project_name = safeGet<std::string>(config, "project_name", "project_name");
        project_description = safeGet<std::string>(config, "project_description", "project_description");
        server_name = safeGet<std::string>(config, "server_name", "server_name", "localhost");
        http_port = safeGet<int>(config, "http_port", "http_port", 8080);

        CROW_LOG_DEBUG << "Project Name: " << project_name;
        CROW_LOG_DEBUG << "Server Name: " << server_name;
        CROW_LOG_DEBUG << "HTTP Port: " << http_port;

        parseHttpsConfig();
        parseConnections();
        parseRateLimitConfig();
        parseAuthConfig();
        parseDuckDBConfig();
        parseTemplateConfig();
        parseGlobalHeartbeatConfig();

        if (config["cache_schema"]) {
            cache_schema = safeGet<std::string>(config, "cache_schema", "cache_schema");
            CROW_LOG_DEBUG << "Cache Schema: " << cache_schema;
        }
    } catch (const std::exception& e) {
        CROW_LOG_ERROR << "Error in parseMainConfig: " << e.what();
        throw;
    }
}

// Template configuration methods
void ConfigManager::parseTemplateConfig() {
    CROW_LOG_INFO << "Parsing template configuration";
    auto template_node = config["template"];
    if (template_node) {
        template_config.path = template_node["path"].as<std::string>();
        std::filesystem::path template_path_relative(template_config.path);
        template_config.path = std::filesystem::absolute((base_path / template_path_relative).lexically_normal()).string();
        CROW_LOG_DEBUG << "Template Path: " << template_config.path;

        if (template_node["environment-whitelist"]) {
            template_config.environment_whitelist = template_node["environment-whitelist"].as<std::vector<std::string>>();
            std::stringstream whitelist_stream;
            for (const auto& item : template_config.environment_whitelist) {
                whitelist_stream << item << " ";
            }   
            CROW_LOG_DEBUG << "Environment Whitelist: " << whitelist_stream.str();
        }
    } else {
        CROW_LOG_ERROR << "Template configuration is missing in flapi.yaml";
        throw std::runtime_error("Template configuration is missing in flapi.yaml");
    }
}

const TemplateConfig& ConfigManager::getTemplateConfig() const {
    return template_config;
}

// DuckDB configuration methods
void ConfigManager::parseDuckDBConfig() {
    CROW_LOG_INFO << "Parsing DuckDB configuration";

    duckdb_config.default_extensions = { "httpfs", "ducklake", "fts", "json", "postgres", "sqlite", "parquet" };
    
    if (config["duckdb"]) {
        auto duckdb_node = config["duckdb"];
        for (const auto& setting : duckdb_node) {
            std::string key = setting.first.as<std::string>();
            std::string value = setting.second.as<std::string>();
            if (key == "db_path") {
                duckdb_config.db_path = value;
                CROW_LOG_DEBUG << "\tDuckDB Path: " << duckdb_config.db_path;
            } else {
                duckdb_config.settings[key] = value;
                CROW_LOG_DEBUG << "\tDuckDB Setting: " << key << " = " << value;
            }
        }
    }
}

const DuckDBConfig& ConfigManager::getDuckDBConfig() const {
    return duckdb_config;
}

std::string ConfigManager::getDuckDBPath() const {
    return duckdb_config.db_path.empty() ? ":memory:" : duckdb_config.db_path;
}

// Endpoint configuration methods
void ConfigManager::loadEndpointConfigsRecursively(const std::filesystem::path& template_path) {
    CROW_LOG_INFO << "Loading endpoint configs recursively from: " << template_path;
    endpoints.clear();

    size_t total_yaml_files = 0;
    size_t loaded_endpoints = 0;

    if (std::filesystem::exists(template_path) && std::filesystem::is_directory(template_path)) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(template_path)) {
            if (entry.is_regular_file()) {
                auto extension = entry.path().extension();
                if (extension == ".yaml" || extension == ".yml") {
                    total_yaml_files++;
                    size_t endpoints_before = endpoints.size();
                    loadEndpointConfig(entry.path().string());
                    if (endpoints.size() > endpoints_before) {
                        loaded_endpoints++;
                    }
                }
            }
        }
    } else {
        CROW_LOG_ERROR << "Template path does not exist or is not a directory: " << template_path;
        throw std::runtime_error("Template path does not exist or is not a directory: " + template_path.string());
    }
    
    size_t skipped_files = total_yaml_files - loaded_endpoints;
    CROW_LOG_INFO << "Loaded " << loaded_endpoints << " endpoint configurations";
    if (skipped_files > 0) {
        CROW_LOG_INFO << "Skipped " << skipped_files << " non-endpoint YAML files (shared configs, templates, etc.)";
    }
}

void ConfigManager::loadEndpointConfig(const std::string& config_file) {
    try {
        CROW_LOG_DEBUG << "\tLoading endpoint config from file: " << config_file;

        // Use ExtendedYamlParser to load the endpoint config file
        auto result = yaml_parser.parseFile(config_file);
        if (!result.success) {
            throw std::runtime_error("Failed to parse endpoint config file: " + result.error_message);
        }
        YAML::Node endpoint_config = result.node;

        EndpointConfig endpoint;

        auto endpoint_dir = std::filesystem::path(config_file).parent_path();

        // Detect configuration type and parse accordingly
        bool is_rest_endpoint = endpoint_config["url-path"].IsDefined();
        bool is_mcp_tool = endpoint_config["mcp-tool"].IsDefined();
        bool is_mcp_resource = endpoint_config["mcp-resource"].IsDefined();
        bool is_mcp_prompt = endpoint_config["mcp-prompt"].IsDefined();

        if (!is_rest_endpoint && !is_mcp_tool && !is_mcp_resource && !is_mcp_prompt) {
            CROW_LOG_DEBUG << "\t\tSkipping non-endpoint configuration file: " << config_file;
            return; // Skip this file - it's likely a shared config or template
        }

        // Parse REST endpoint specific fields (optional)
        if (is_rest_endpoint) {
            endpoint.urlPath = safeGet<std::string>(endpoint_config, "url-path", "url-path");
            endpoint.method = safeGet<std::string>(endpoint_config, "method", "method", "GET");
            endpoint.with_pagination = safeGet<bool>(endpoint_config, "with-pagination", "with-pagination", true);
            endpoint.request_fields_validation = safeGet<bool>(endpoint_config, "request-fields-validation", "request-fields-validation", false);

            CROW_LOG_DEBUG << "\t\tREST Endpoint: " << endpoint.method << " " << endpoint.urlPath;
        }

        // Parse MCP tool specific fields (optional)
        if (is_mcp_tool) {
            auto mcp_tool_node = endpoint_config["mcp-tool"];
            EndpointConfig::MCPToolInfo tool_info;
            tool_info.name = safeGet<std::string>(mcp_tool_node, "name", "mcp-tool.name");
            tool_info.description = safeGet<std::string>(mcp_tool_node, "description", "mcp-tool.description");
            tool_info.result_mime_type = safeGet<std::string>(mcp_tool_node, "result_mime_type", "mcp-tool.result_mime_type", "application/json");
            endpoint.mcp_tool = tool_info;

            CROW_LOG_DEBUG << "\t\tMCP Tool: " << tool_info.name;
        }

        // Parse MCP resource specific fields (optional)
        if (is_mcp_resource) {
            auto mcp_resource_node = endpoint_config["mcp-resource"];
            EndpointConfig::MCPResourceInfo resource_info;
            resource_info.name = safeGet<std::string>(mcp_resource_node, "name", "mcp-resource.name");
            resource_info.description = safeGet<std::string>(mcp_resource_node, "description", "mcp-resource.description");
            resource_info.mime_type = safeGet<std::string>(mcp_resource_node, "mime_type", "mcp-resource.mime_type", "application/json");
            endpoint.mcp_resource = resource_info;

            CROW_LOG_DEBUG << "\t\tMCP Resource: " << resource_info.name;
        }

        // Parse MCP prompt specific fields (optional)
        if (is_mcp_prompt) {
            auto mcp_prompt_node = endpoint_config["mcp-prompt"];
            EndpointConfig::MCPPromptInfo prompt_info;
            prompt_info.name = safeGet<std::string>(mcp_prompt_node, "name", "mcp-prompt.name");
            prompt_info.description = safeGet<std::string>(mcp_prompt_node, "description", "mcp-prompt.description");

            // Parse template content
            if (mcp_prompt_node["template"]) {
                prompt_info.template_content = mcp_prompt_node["template"].as<std::string>();
            } else {
                throw std::runtime_error("MCP prompt must have a 'template' field");
            }

            // Parse arguments (optional)
            if (mcp_prompt_node["arguments"]) {
                for (const auto& arg : mcp_prompt_node["arguments"]) {
                    prompt_info.arguments.push_back(arg.as<std::string>());
                }
            }

            endpoint.mcp_prompt = prompt_info;

            CROW_LOG_DEBUG << "\t\tMCP Prompt: " << prompt_info.name;
        }

        // Parse common fields
        // For MCP prompts, template content is embedded in the config, not in a separate file
        if (!is_mcp_prompt) {
            endpoint.templateSource = (endpoint_dir / safeGet<std::string>(endpoint_config, "template-source", "template-source")).string();
            CROW_LOG_DEBUG << "\t\tTemplate Source: " << endpoint.templateSource;
        } else {
            // For MCP prompts, template content is already parsed above
            CROW_LOG_DEBUG << "\t\tTemplate Content: embedded in config";
        }

        parseEndpointRequestFields(endpoint_config, endpoint);
        parseEndpointConnection(endpoint_config, endpoint);
        parseEndpointRateLimit(endpoint_config, endpoint);
        parseEndpointAuth(endpoint_config, endpoint);
        parseEndpointCache(endpoint_config, endpoint_dir, endpoint);
        parseEndpointHeartbeat(endpoint_config, endpoint);

        endpoints.push_back(endpoint);

        // Log configuration summary
        std::string config_summary = "\t\tConfiguration loaded: ";
        if (endpoint.isRESTEndpoint()) config_summary += "REST(" + endpoint.urlPath + ") ";
        if (endpoint.isMCPTool()) config_summary += "MCP-Tool(" + endpoint.mcp_tool->name + ") ";
        if (endpoint.isMCPResource()) config_summary += "MCP-Resource(" + endpoint.mcp_resource->name + ") ";
        if (endpoint.isMCPPrompt()) config_summary += "MCP-Prompt(" + endpoint.mcp_prompt->name + ") ";
        CROW_LOG_DEBUG << config_summary;
    } catch (const std::exception& e) {
        throw std::runtime_error("Error loading endpoint config from file: " + config_file + ", Error: " + std::string(e.what()));
    }
}

void ConfigManager::parseEndpointRequestFields(const YAML::Node& endpoint_config, EndpointConfig& endpoint) {
    if (endpoint_config["request"]) {
        for (const auto& req : endpoint_config["request"]) {
            RequestFieldConfig field;
            field.fieldName = safeGet<std::string>(req, "field-name", "request.field-name");
            field.fieldIn = safeGet<std::string>(req, "field-in", "request.field-in");
            field.description = safeGet<std::string>(req, "description", "request.description", "");
            field.required = safeGet<bool>(req, "required", "request.required", false);
            
            if (req["default"]) {
                if (req["default"].IsScalar()) {
                    field.defaultValue = req["default"].as<std::string>();
                } else {
                    CROW_LOG_WARNING << "Default value for field " << field.fieldName << " must be a scalar value";
                }
            }
            
            parseEndpointValidators(req, field);
            
            endpoint.request_fields.push_back(field);
        }
    }
}

void ConfigManager::parseEndpointValidators(const YAML::Node& req, RequestFieldConfig& field) {
    if (req["validators"]) {
        for (const auto& validator : req["validators"]) {
            ValidatorConfig validatorConfig;
            validatorConfig.type = safeGet<std::string>(validator, "type", "validators.type");
            
            if (validatorConfig.type == "int") {
                validatorConfig.min = safeGet<int>(validator, "min", "validators.min", std::numeric_limits<int>::min());
                validatorConfig.max = safeGet<int>(validator, "max", "validators.max", std::numeric_limits<int>::max());
            } else if (validatorConfig.type == "string") {
                validatorConfig.regex = safeGet<std::string>(validator, "regex", "validators.regex", "");
            } else if (validatorConfig.type == "enum") {
                validatorConfig.allowedValues = safeGet<std::vector<std::string>>(validator, "allowedValues", "validators.allowedValues");
            } else if (validatorConfig.type == "date") {
                validatorConfig.minDate = safeGet<std::string>(validator, "min", "validators.min", "");
                validatorConfig.maxDate = safeGet<std::string>(validator, "max", "validators.max", "");
            } else if (validatorConfig.type == "time") {
                validatorConfig.minTime = safeGet<std::string>(validator, "min", "validators.min", "");
                validatorConfig.maxTime = safeGet<std::string>(validator, "max", "validators.max", "");
            }
            
            validatorConfig.preventSqlInjection = safeGet<bool>(validator, "preventSqlInjection", "validators.preventSqlInjection", true);
            field.validators.push_back(validatorConfig);
        }
    }
}

void ConfigManager::parseEndpointConnection(const YAML::Node& endpoint_config, EndpointConfig& endpoint) {
    if (endpoint_config["connection"]) {
        endpoint.connection = safeGet<std::vector<std::string>>(endpoint_config, "connection", "connection");
    }
}

void ConfigManager::parseEndpointRateLimit(const YAML::Node& endpoint_config, EndpointConfig& endpoint) {
    if (endpoint_config["rate-limit"]) {
        auto rate_limit_node = endpoint_config["rate-limit"];
        endpoint.rate_limit.enabled = safeGet<bool>(rate_limit_node, "enabled", "rate-limit.enabled", false);
        endpoint.rate_limit.max = safeGet<int>(rate_limit_node, "max", "rate-limit.max", 100);
        endpoint.rate_limit.interval = safeGet<int>(rate_limit_node, "interval", "rate-limit.interval", 60);
    }
}

void ConfigManager::parseEndpointAuth(const YAML::Node& endpoint_config, EndpointConfig& endpoint) {
    CROW_LOG_DEBUG << "\tParsing endpoint auth configuration";
    if (endpoint_config["auth"]) {
        auto auth_node = endpoint_config["auth"];
        endpoint.auth.enabled = safeGet<bool>(auth_node, "enabled", "auth.enabled", false);
        endpoint.auth.type = safeGet<std::string>(auth_node, "type", "auth.type", "");
        
        // Parse AWS Secrets Manager configuration if present
        if (auth_node["from-aws-secretmanager"]) {
            CROW_LOG_DEBUG << "\t\tParsing AWS Secrets Manager configuration";
            auto aws_node = auth_node["from-aws-secretmanager"];
            
            AuthFromSecretManagerConfig aws_config;
            aws_config.secret_name = safeGet<std::string>(aws_node, "secret_name", "auth.from-aws-secretmanager.secret_name");
            if (aws_node["region"]) {
                aws_config.region = safeGet<std::string>(aws_node, "region", "auth.from-aws-secretmanager.region");
            }

            if (aws_node["secret_id"]) {
                aws_config.secret_id = safeGet<std::string>(aws_node, "secret_id", "auth.from-aws-secretmanager.secret_id");
            }

            if (aws_node["secret_key"]) {
                aws_config.secret_key = safeGet<std::string>(aws_node, "secret_key", "auth.from-aws-secretmanager.secret_key");
            }

            aws_config.secret_table = safeGet<std::string>(aws_node, "secret_table", 
                                                           "auth.from-aws-secretmanager.secret_table", 
                                                           secretNameToTableName(aws_config.secret_name));
            
            aws_config.init = safeGet<std::string>(aws_node, "init", 
                                                  "auth.from-aws-secretmanager.init", 
                                                  createDefaultAuthInit(aws_config.secret_name, 
                                                                        aws_config.region, 
                                                                        aws_config.secret_id, 
                                                                        aws_config.secret_key));

            endpoint.auth.from_aws_secretmanager = aws_config;
            
            CROW_LOG_DEBUG << "\t\tAWS Secrets Manager configuration:";
            CROW_LOG_DEBUG << "\t\t\tSecret Name: " << aws_config.secret_name;
            CROW_LOG_DEBUG << "\t\t\tRegion: " << aws_config.region;
            CROW_LOG_DEBUG << "\t\t\tSecret Table: " << aws_config.secret_table;
            CROW_LOG_DEBUG << "\t\t\tInit: " << aws_config.init;
            CROW_LOG_DEBUG << "\t\t\tSecret ID: *****[" << aws_config.secret_id.length() << "]";
            CROW_LOG_DEBUG << "\t\t\tSecret Key: *****[" << aws_config.secret_key.length() << "]";
        }

        // Parse inline users if present
        else if (auth_node["users"]) {
            CROW_LOG_DEBUG << "\t\tParsing inline users configuration";
            for (const auto& user : auth_node["users"]) {
                AuthUser auth_user;
                auth_user.username = safeGet<std::string>(user, "username", "auth.users.username");
                auth_user.password = safeGet<std::string>(user, "password", "auth.users.password");
                auth_user.roles = safeGet<std::vector<std::string>>(user, "roles", "auth.users.roles", std::vector<std::string>());
                endpoint.auth.users.push_back(auth_user);
                CROW_LOG_DEBUG << "\t\t\tAdded user: " << auth_user.username << " with " << auth_user.roles.size() << " roles";
            }
        }
    }
}

std::string ConfigManager::secretNameToTableName(const std::string& secret_name) 
{   
    // Sanitize the secret name, such that it can be a SQL table
    std::string sanitized_name = secret_name;
     std::replace_if(sanitized_name.begin(), sanitized_name.end(), 
                    [](char c) { return !std::isalnum(c); }, '_');
    return "auth_" + sanitized_name;
}

std::string ConfigManager::secretNameToSecretId(const std::string& secret_name) 
{
    // Sanitize the secret name, such that it can be duckdb secret identifier
    std::string sanitized_name = secret_name;
    std::replace_if(sanitized_name.begin(), sanitized_name.end(), 
                    [](char c) { return !std::isalnum(c); }, '_');
    return sanitized_name;
}

std::string ConfigManager::createDefaultAuthInit(const std::string& secret_name, 
                                                 const std::string& region, 
                                                 const std::string& secret_id, 
                                                 const std::string& secret_key) 
{
    std::stringstream init;
    init << "CREATE OR REPLACE SECRET " << secretNameToSecretId(secret_name) << " ";
    init << "(TYPE S3";

    if (!secret_id.empty() && !secret_key.empty()) {
        init << ", KEY_ID '" << secret_id << "', SECRET '" << secret_key << "'";
    } else {
        init << ", PROVIDER CREDENTIAL_CHAIN";
    }

    if (!region.empty()) {
        init << ", REGION '" << region << "'";
    }

    init << ");";

    return init.str();
}

void ConfigManager::parseEndpointCache(const YAML::Node& endpoint_config, const std::filesystem::path& endpoint_dir, EndpointConfig& endpoint) {
    CROW_LOG_DEBUG << "\tParsing endpoint cache configuration";
    if (endpoint_config["cache"]) {
        auto cache_node = endpoint_config["cache"];
        
        // Parse enabled field - if not explicitly set, enable cache if any cache configuration is present
        endpoint.cache.enabled = safeGet<bool>(cache_node, "enabled", "cache.enabled", true);
        
        endpoint.cache.cacheTableName = safeGet<std::string>(cache_node, "cache-table-name", "cache.cache-table-name", "");
        endpoint.cache.cacheSource = (endpoint_dir / safeGet<std::string>(cache_node, "cache-source", "cache.cache-source")).string();
        endpoint.cache.refreshTime = safeGet<std::string>(cache_node, "refresh-time", "cache.refresh-time", "");
        endpoint.cache.refreshEndpoint = safeGet<bool>(cache_node, "refresh-endpoint", "cache.refresh-endpoint", false);
        endpoint.cache.maxPreviousTables = safeGet<std::size_t>(cache_node, "max-previous-tables", "cache.max-previous-tables", 5);

        CROW_LOG_DEBUG << "\t\tCache Enabled: " << (endpoint.cache.enabled ? "true" : "false");
        CROW_LOG_DEBUG << "\t\tCache Table Name: " << endpoint.cache.cacheTableName;
        CROW_LOG_DEBUG << "\t\tCache Source: " << endpoint.cache.cacheSource;
        CROW_LOG_DEBUG << "\t\tRefresh Time: " << endpoint.cache.refreshTime;
        CROW_LOG_DEBUG << "\t\tRefresh Endpoint: " << (endpoint.cache.refreshEndpoint ? "true" : "false");
        CROW_LOG_DEBUG << "\t\tMax Previous Tables: " << endpoint.cache.maxPreviousTables;
    }
}

void ConfigManager::parseEndpointHeartbeat(const YAML::Node& endpoint_config, EndpointConfig& endpoint) {
    CROW_LOG_DEBUG << "\tParsing endpoint heartbeat configuration";
    if (endpoint_config["heartbeat"]) {
        auto heartbeat_node = endpoint_config["heartbeat"];
        endpoint.heartbeat.enabled = safeGet<bool>(heartbeat_node, "enabled", "heartbeat.enabled", false);
        if (endpoint.heartbeat.enabled) {
            CROW_LOG_DEBUG << "\t\tHeartbeat enabled for endpoint: " << endpoint.urlPath;
        }

        if (heartbeat_node["params"]) {
            for (const auto& param : heartbeat_node["params"]) {
                endpoint.heartbeat.params[param.first.as<std::string>()] = param.second.as<std::string>();
            }
        }
    }
}

// Connection configuration methods
void ConfigManager::parseConnections() {
    CROW_LOG_INFO << "Parsing connections";
    if (config["connections"]) {
        auto connections_node = config["connections"];
        for (const auto& connection : connections_node) {
            std::string name = connection.first.as<std::string>();
            CROW_LOG_DEBUG << "Parsing connection: " << name;
            ConnectionConfig conn_config;
            
            conn_config.init = safeGet<std::string>(connection.second, "init", "connections." + name + ".init", "");
            conn_config.log_queries = safeGet<bool>(connection.second, "log-queries", "connections." + name + ".log-queries", false);
            conn_config.log_parameters = safeGet<bool>(connection.second, "log-parameters", "connections." + name + ".log-parameters", false);
            
            CROW_LOG_DEBUG << "Connection " << name << ": log_queries=" << conn_config.log_queries 
                           << ", log_parameters=" << conn_config.log_parameters;

            if (connection.second["properties"]) {
                auto properties = connection.second["properties"];
                for (const auto& prop : properties) {
                    conn_config.properties[prop.first.as<std::string>()] = prop.second.as<std::string>();
                }
            }

            connections[name] = conn_config;
        }
    }
    CROW_LOG_INFO << "Parsed " << connections.size() << " connections";
}

// Rate limit configuration methods
void ConfigManager::parseRateLimitConfig() {
    CROW_LOG_INFO << "Parsing rate limit configuration";
    if (config["rate_limit"]) {
        auto rate_limit_node = config["rate_limit"];
        rate_limit_config.enabled = rate_limit_node["enabled"].as<bool>();
        rate_limit_config.max = rate_limit_node["max"].as<int>();
        rate_limit_config.interval = rate_limit_node["interval"].as<int>();
        CROW_LOG_DEBUG << "Rate Limit: enabled=" << rate_limit_config.enabled 
                       << ", max=" << rate_limit_config.max 
                       << ", interval=" << rate_limit_config.interval;
    }
}

// Auth configuration methods
void ConfigManager::parseAuthConfig() {
    CROW_LOG_INFO << "Parsing auth configuration";
    if (config["auth"]) {
        auto auth_node = config["auth"];
        auth_enabled = auth_node["enabled"].as<bool>();
        CROW_LOG_DEBUG << "Auth enabled: " << auth_enabled;
        if (auth_enabled) {
            AuthConfig auth_config;
            auth_config.type = auth_node["type"].as<std::string>();
            auth_config.jwt_secret = auth_node["jwt_secret"].as<std::string>();
            auth_config.jwt_issuer = auth_node["jwt_issuer"].as<std::string>();
            CROW_LOG_DEBUG << "Auth type: " << auth_config.type;
            CROW_LOG_DEBUG << "JWT issuer: " << auth_config.jwt_issuer;

            if (auth_node["users"]) {
                for (const auto& user : auth_node["users"]) {
                    AuthUser auth_user;
                    auth_user.username = user["username"].as<std::string>();
                    auth_user.password = user["password"].as<std::string>();
                    if (user["roles"]) {
                        auth_user.roles = user["roles"].as<std::vector<std::string>>();
                    }
                    auth_config.users.push_back(auth_user);
                    CROW_LOG_DEBUG << "Added user: " << auth_user.username << " with " << auth_user.roles.size() << " roles";
                }
            }
        }
    }
}

// HTTPS configuration methods
void ConfigManager::parseHttpsConfig() {
    if (config["enforce-https"]) {
        auto https_node = config["enforce-https"];
        if (https_node.IsMap()) {
            https_config.enabled = safeGet<bool>(https_node, "enabled", "enforce-https.enabled", false);
            
            if (https_config.enabled) {
                https_config.ssl_cert_file = safeGet<std::string>(https_node, "ssl-cert-file", "enforce-https.ssl-cert-file", "");
                https_config.ssl_key_file = safeGet<std::string>(https_node, "ssl-key-file", "enforce-https.ssl-key-file", "");

                if (https_config.ssl_cert_file.empty() || https_config.ssl_key_file.empty()) {
                    throw ConfigurationError("SSL certificate and key files must be specified when HTTPS is enabled", "enforce-https");
                }
            }
        } else {
            throw ConfigurationError("'enforce-https' must be a map", "enforce-https");
        }
    } else {
        https_config.enabled = false;
    }
}

// Global heartbeat configuration methods
void ConfigManager::parseGlobalHeartbeatConfig() {
    CROW_LOG_INFO << "Parsing global heartbeat configuration";
    if (config["heartbeat"]) {
        auto heartbeat_node = config["heartbeat"];

        global_heartbeat_config.enabled = safeGet<bool>(heartbeat_node, "enabled", "heartbeat.enabled", false);
        if (global_heartbeat_config.enabled) {
            CROW_LOG_DEBUG << "Global heartbeat enabled";
        }

        global_heartbeat_config.workerInterval = std::chrono::seconds(
            safeGet<int>(heartbeat_node, "worker-interval", "heartbeat.worker-interval", 10)
        );
        CROW_LOG_DEBUG << "Global heartbeat worker interval: " << global_heartbeat_config.workerInterval.count() << " seconds";
    }
}

// Utility methods
std::string ConfigManager::makePathRelativeToBasePathIfNecessary(const std::string& value) const {
    if (value.find("./") == 0 || value.find("../") == 0) {
        std::filesystem::path relativePath(value);
        std::filesystem::path absolutePath = std::filesystem::absolute(base_path / relativePath).lexically_normal();
        return absolutePath.string();
    }
    return value;
}

std::unordered_map<std::string, std::string> ConfigManager::getPropertiesForTemplates(const std::string& connectionName) const {
    // Add validation for empty connection name
    if (connectionName.empty()) {
        CROW_LOG_WARNING << "getPropertiesForTemplates called with empty connection name";
        return {};
    }

    // Add validation for connections map
    if (connections.empty()) {
        CROW_LOG_WARNING << "getPropertiesForTemplates called with empty connections map";
        return {};
    }

    // Use find() instead of at() to avoid potential exceptions
    auto connIt = connections.find(connectionName);
    if (connIt == connections.end()) {
        CROW_LOG_WARNING << "No connection found for name: " << connectionName;
        return {};
    }

    try {
        const auto& props = connIt->second.properties;
        std::unordered_map<std::string, std::string> propsForTemplates;

        for (const auto& [key, value] : props) {
            if (key.empty()) {
                CROW_LOG_WARNING << "Empty key found in properties for connection: " << connectionName;
                continue;
            }
            try {
                propsForTemplates[key] = makePathRelativeToBasePathIfNecessary(value);
            } catch (const std::exception& e) {
                CROW_LOG_ERROR << "Error processing property " << key << ": " << e.what();
                continue;
            }
        }

        return propsForTemplates;
    } catch (const std::exception& e) {
        CROW_LOG_ERROR << "Exception in getPropertiesForTemplates: " << e.what();
        return {};
    }
}

std::string ConfigManager::getFullCacheSourcePath(const EndpointConfig& endpoint) const {
    if (endpoint.cache.cacheSource.empty()) {
        return "";
    }
    return (base_path / endpoint.cache.cacheSource).string();
}

void ConfigManager::printConfig() const {
    std::cout << "Current configuration structure:" << std::endl;
    printYamlNode(config);
}

void ConfigManager::printYamlNode(const YAML::Node& node, int indent) {
    std::string indent_str(indent * 2, ' ');
    
    switch (node.Type()) {
        case YAML::NodeType::Null:
            std::cout << indent_str << "(null)" << std::endl;
            break;
        case YAML::NodeType::Scalar:
            std::cout << indent_str << node.Scalar() << std::endl;
            break;
        case YAML::NodeType::Sequence:
            for (std::size_t i = 0; i < node.size(); i++) {
                std::cout << indent_str << "- ";
                printYamlNode(node[i], indent + 1);
            }
            break;
        case YAML::NodeType::Map:
            for (YAML::const_iterator it = node.begin(); it != node.end(); ++it) {
                std::cout << indent_str << it->first.Scalar() << ": ";
                if (it->second.IsScalar()) {
                    std::cout << it->second.Scalar() << std::endl;
                } else {
                    std::cout << std::endl;
                    printYamlNode(it->second, indent + 1);
                }
            }
            break;
        default:
            std::cout << indent_str << "Unknown YAML node type" << std::endl;
    }
}

// Safe getter methods
template<typename T>
T ConfigManager::safeGet(const YAML::Node& node, const std::string& key, const std::string& path, const T& defaultValue) const {
    if (!node[key]) {
        return defaultValue;
    }
    try {
        return node[key].as<T>();
    } catch (const YAML::Exception& e) {
        throw ConfigurationError("Invalid value for key: " + key + ", Error: " + e.what(), path);
    }
}

template<typename T>
T ConfigManager::safeGet(const YAML::Node& node, const std::string& key, const std::string& path) const {
    if (!node[key]) {
        throw ConfigurationError("Missing required key: " + key, path);
    }
    try {
        return node[key].as<T>();
    } catch (const YAML::Exception& e) {
        throw ConfigurationError("Invalid value for key: " + key + ", Error: " + e.what(), path);
    }
}

// Getter methods
std::string ConfigManager::getProjectName() const { return project_name; }
std::string ConfigManager::getProjectDescription() const { return project_description; }
std::string ConfigManager::getServerName() const { return server_name; }
int ConfigManager::getHttpPort() const { return http_port; }
void ConfigManager::setHttpPort(int port) { http_port = port; }
std::string ConfigManager::getTemplatePath() const { return template_config.path; }
std::string ConfigManager::getCacheSchema() const { return cache_schema; }
const std::unordered_map<std::string, ConnectionConfig>& ConfigManager::getConnections() const { return connections; }
const RateLimitConfig& ConfigManager::getRateLimitConfig() const { return rate_limit_config; }
bool ConfigManager::isHttpsEnforced() const { return https_config.enabled; }
bool ConfigManager::isAuthEnabled() const { return auth_enabled; }
const std::vector<EndpointConfig>& ConfigManager::getEndpoints() const { return endpoints; }
std::string ConfigManager::getBasePath() const { return base_path.string(); }

// JSON configuration methods
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
    result["auth"]["enabled"] = auth_enabled;
    
    return result;
}

crow::json::wvalue ConfigManager::getEndpointsConfig() const {
    crow::json::wvalue endpointsJson;
    for (const auto& endpoint : endpoints) {
        endpointsJson[endpoint.urlPath] = serializeEndpointConfig(endpoint, EndpointJsonStyle::CamelCase);
    }
    return endpointsJson;
}

crow::json::wvalue ConfigManager::serializeEndpointConfig(const EndpointConfig& config, EndpointJsonStyle style) const {
    crow::json::wvalue json = crow::json::wvalue::object();

    auto set = [&](const std::string& hyphen_key, const std::string& camel_key, const auto& value) {
        if (style == EndpointJsonStyle::HyphenCase) {
            json[hyphen_key] = value;
        } else {
            json[camel_key] = value;
        }
    };

    if (config.isRESTEndpoint()) {
        set("url-path", "urlPath", config.urlPath);
        set("method", "method", config.method);
        set("template-source", "templateSource", config.templateSource);

        if (!config.connection.empty()) {
            auto connection_list = crow::json::wvalue::list();
            for (const auto& conn : config.connection) {
                connection_list.push_back(conn);
            }
            json["connection"] = std::move(connection_list);
        }

        set("with-pagination", "withPagination", config.with_pagination);
        set("request-fields-validation", "requestFieldsValidation", config.request_fields_validation);
    }

    std::vector<crow::json::wvalue> requestFields;
    for (const auto& field : config.request_fields) {
            crow::json::wvalue fieldJson;
        if (style == EndpointJsonStyle::HyphenCase) {
            fieldJson["field-name"] = field.fieldName;
            fieldJson["field-in"] = field.fieldIn;
        } else {
            fieldJson["fieldName"] = field.fieldName;
            fieldJson["fieldIn"] = field.fieldIn;
        }
        fieldJson[(style == EndpointJsonStyle::HyphenCase) ? "description" : "description"] = field.description;
        fieldJson[(style == EndpointJsonStyle::HyphenCase) ? "required" : "required"] = field.required;
        if (!field.defaultValue.empty()) {
            fieldJson[(style == EndpointJsonStyle::HyphenCase) ? "default" : "defaultValue"] = field.defaultValue;
        }

            std::vector<crow::json::wvalue> validatorsJson;
        for (const auto& validator : field.validators) {
                crow::json::wvalue validatorJson;
            validatorJson[(style == EndpointJsonStyle::HyphenCase) ? "type" : "type"] = validator.type;
                if (validator.type == "string") {
                validatorJson[(style == EndpointJsonStyle::HyphenCase) ? "regex" : "regex"] = validator.regex;
                } else if (validator.type == "int") {
                validatorJson[(style == EndpointJsonStyle::HyphenCase) ? "min" : "min"] = validator.min;
                validatorJson[(style == EndpointJsonStyle::HyphenCase) ? "max" : "max"] = validator.max;
                }
                validatorsJson.push_back(std::move(validatorJson));
            }
        fieldJson[(style == EndpointJsonStyle::HyphenCase) ? "validators" : "validators"] = std::move(validatorsJson);
        requestFields.push_back(std::move(fieldJson));
    }

    auto request_list = crow::json::wvalue::list();
    for (auto& field_json : requestFields) {
        request_list.push_back(std::move(field_json));
    }
    if (!request_list.empty()) {
        json["request"] = std::move(request_list);
    }

    // Always include auth section
    crow::json::wvalue authJson;
    authJson["enabled"] = config.auth.enabled;
    authJson["type"] = config.auth.type;
    if (config.auth.from_aws_secretmanager) {
        crow::json::wvalue awsJson;
        awsJson[(style == EndpointJsonStyle::HyphenCase) ? "secret_name" : "secretName"] = config.auth.from_aws_secretmanager->secret_name;
        awsJson[(style == EndpointJsonStyle::HyphenCase) ? "region" : "region"] = config.auth.from_aws_secretmanager->region;
        authJson[(style == EndpointJsonStyle::HyphenCase) ? "from-aws-secretmanager" : "fromAwsSecretmanager"] = std::move(awsJson);
    }
    json["auth"] = std::move(authJson);

    // Always include cache section
    crow::json::wvalue cacheJson;
    cacheJson["enabled"] = config.cache.enabled;
    cacheJson[(style == EndpointJsonStyle::HyphenCase) ? "cache-source" : "cacheSource"] = config.cache.cacheSource;
    cacheJson[(style == EndpointJsonStyle::HyphenCase) ? "cache-table" : "cacheTable"] = config.cache.cacheTableName;
    cacheJson[(style == EndpointJsonStyle::HyphenCase) ? "refresh-time" : "refreshTime"] = config.cache.refreshTime;
    json[(style == EndpointJsonStyle::HyphenCase) ? "cache" : "cache"] = std::move(cacheJson);

    if (config.isMCPTool()) {
        crow::json::wvalue toolJson;
        toolJson[(style == EndpointJsonStyle::HyphenCase) ? "name" : "name"] = config.mcp_tool->name;
        toolJson[(style == EndpointJsonStyle::HyphenCase) ? "description" : "description"] = config.mcp_tool->description;
        json[(style == EndpointJsonStyle::HyphenCase) ? "mcp-tool" : "mcpTool"] = std::move(toolJson);
    }
    if (config.isMCPResource()) {
        crow::json::wvalue resourceJson;
        resourceJson[(style == EndpointJsonStyle::HyphenCase) ? "name" : "name"] = config.mcp_resource->name;
        resourceJson[(style == EndpointJsonStyle::HyphenCase) ? "description" : "description"] = config.mcp_resource->description;
        json[(style == EndpointJsonStyle::HyphenCase) ? "mcp-resource" : "mcpResource"] = std::move(resourceJson);
    }
    if (config.isMCPPrompt()) {
        crow::json::wvalue promptJson;
        promptJson[(style == EndpointJsonStyle::HyphenCase) ? "name" : "name"] = config.mcp_prompt->name;
        promptJson[(style == EndpointJsonStyle::HyphenCase) ? "description" : "description"] = config.mcp_prompt->description;
        promptJson[(style == EndpointJsonStyle::HyphenCase) ? "template" : "template"] = config.mcp_prompt->template_content;
        json[(style == EndpointJsonStyle::HyphenCase) ? "mcp-prompt" : "mcpPrompt"] = std::move(promptJson);
    }

    // Always include rate-limit section
    crow::json::wvalue rateJson;
    rateJson["enabled"] = config.rate_limit.enabled;
    rateJson[(style == EndpointJsonStyle::HyphenCase) ? "max" : "max"] = config.rate_limit.max;
    rateJson[(style == EndpointJsonStyle::HyphenCase) ? "interval" : "interval"] = config.rate_limit.interval;
    json[(style == EndpointJsonStyle::HyphenCase) ? "rate-limit" : "rateLimit"] = std::move(rateJson);

    // Always include heartbeat section
    crow::json::wvalue heartbeatJson;
    heartbeatJson["enabled"] = config.heartbeat.enabled;
    json["heartbeat"] = std::move(heartbeatJson);

    return json;
}

namespace {
std::string firstExistingKey(const crow::json::rvalue& json, std::initializer_list<std::string> keys) {
    for (const auto& key : keys) {
        if (json.has(key)) {
            return key;
        }
    }
    return {};
}

std::string requireStringField(const crow::json::rvalue& json, std::initializer_list<std::string> keys) {
    auto key = firstExistingKey(json, keys);
    if (key.empty()) {
        throw std::runtime_error("Missing required field in endpoint config");
    }
    return key;
}
}

EndpointConfig ConfigManager::deserializeEndpointConfig(const crow::json::rvalue& json) const {
    EndpointConfig config;

    auto getString = [&](std::initializer_list<std::string> keys) -> std::string {
        auto key = firstExistingKey(json, keys);
        if (!key.empty()) {
            return json[key].s();
        }
        return {};
    };

    auto getBool = [&](std::initializer_list<std::string> keys, bool defaultValue) -> bool {
        auto key = firstExistingKey(json, keys);
        if (!key.empty()) {
            return json[key].b();
        }
        return defaultValue;
    };

    auto getList = [&](std::initializer_list<std::string> keys) -> std::vector<std::string> {
        std::vector<std::string> result;
        auto key = firstExistingKey(json, keys);
        if (!key.empty()) {
            for (const auto& item : json[key]) {
                result.push_back(item.s());
            }
        }
        return result;
    };

    auto urlKey = requireStringField(json, {"url-path", "urlPath", "url_path"});
    config.urlPath = json[urlKey].s();
    auto methodKey = firstExistingKey(json, {"method", "Method"});
    std::string method = methodKey.empty() ? std::string("GET") : std::string(json[methodKey].s());
    config.method = std::move(method);
    auto templateKey = requireStringField(json, {"template-source", "templateSource", "template_source"});
    config.templateSource = json[templateKey].s();
    config.connection = getList({"connection", "connections"});
    config.with_pagination = getBool({"with-pagination", "withPagination", "with_pagination"}, true);
    config.request_fields_validation = getBool({"request-fields-validation", "requestFieldsValidation"}, false);

    if (json.has("request")) {
        for (const auto& field : json["request"]) {
            RequestFieldConfig fieldConfig;
            auto fieldNameKey = requireStringField(field, {"field-name", "fieldName"});
            auto fieldInKey = requireStringField(field, {"field-in", "fieldIn"});
            fieldConfig.fieldName = field[fieldNameKey].s();
            fieldConfig.fieldIn = field[fieldInKey].s();
            auto descKey = firstExistingKey(field, {"description"});
            if (!descKey.empty()) {
                fieldConfig.description = field[descKey].s();
            }
            auto requiredKey = firstExistingKey(field, {"required"});
            fieldConfig.required = !requiredKey.empty() ? field[requiredKey].b() : false;
            config.request_fields.push_back(fieldConfig);
        }
    }

    if (json.has("cache") || json.has("cache-config") || json.has("cacheConfig")) {
        auto key = firstExistingKey(json, {"cache", "cache-config", "cacheConfig"});
        const auto& cacheJson = json[key];
        if (cacheJson.has("enabled")) {
            config.cache.enabled = cacheJson["enabled"].b();
        }
        auto cacheSourceKey = firstExistingKey(cacheJson, {"cache-source", "cacheSource"});
        if (!cacheSourceKey.empty()) {
            config.cache.cacheSource = cacheJson[cacheSourceKey].s();
        }
        auto cacheTableKey = firstExistingKey(cacheJson, {"cache-table", "cacheTable"});
        if (!cacheTableKey.empty()) {
            config.cache.cacheTableName = cacheJson[cacheTableKey].s();
        }
        auto refreshKey = firstExistingKey(cacheJson, {"refresh-time", "refreshTime"});
        if (!refreshKey.empty()) {
            config.cache.refreshTime = cacheJson[refreshKey].s();
        }
    }

    if (json.has("auth")) {
        const auto& authJson = json["auth"];
        config.auth.enabled = authJson.has("enabled") ? authJson["enabled"].b() : false;
        if (authJson.has("type")) {
            config.auth.type = authJson["type"].s();
        }
    }

    return config;
}

// MCP configuration methods

// Other methods
void ConfigManager::refreshConfig() {
    throw std::runtime_error("Not implemented");
}

void ConfigManager::addEndpoint(const EndpointConfig& endpoint) {
    endpoints.push_back(endpoint);
}

bool ConfigManager::removeEndpointByPath(const std::string& path) {
    auto before = endpoints.size();
    endpoints.erase(
        std::remove_if(endpoints.begin(), endpoints.end(), [&](const EndpointConfig& endpoint) {
            if (endpoint.isRESTEndpoint()) {
                std::vector<std::string> param_names;
                std::map<std::string, std::string> path_params;
                if (RouteTranslator::matchAndExtractParams(endpoint.urlPath, path, param_names, path_params)) {
                    return true;
                }
            }

            if (endpoint.isMCPTool() && endpoint.mcp_tool->name == path) {
                return true;
            }
            if (endpoint.isMCPResource() && endpoint.mcp_resource->name == path) {
                return true;
            }
            if (endpoint.isMCPPrompt() && endpoint.mcp_prompt->name == path) {
                return true;
            }

            return false;
        }),
        endpoints.end());
    return before != endpoints.size();
}

bool ConfigManager::replaceEndpoint(const EndpointConfig& endpoint) {
    for (auto& candidate : endpoints) {
        if (endpoint.isRESTEndpoint() && candidate.isRESTEndpoint() && candidate.urlPath == endpoint.urlPath) {
            candidate = endpoint;
            return true;
        }
        if (endpoint.isMCPTool() && candidate.isMCPTool() && candidate.mcp_tool->name == endpoint.mcp_tool->name) {
            candidate = endpoint;
            return true;
        }
        if (endpoint.isMCPResource() && candidate.isMCPResource() && candidate.mcp_resource->name == endpoint.mcp_resource->name) {
            candidate = endpoint;
            return true;
        }
        if (endpoint.isMCPPrompt() && candidate.isMCPPrompt() && candidate.mcp_prompt->name == endpoint.mcp_prompt->name) {
            candidate = endpoint;
            return true;
        }
    }
    return false;
}

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



// Template method implementations
template<typename T>
T ConfigManager::getValueOrThrow(const YAML::Node& node, const std::string& key, const std::string& yamlPath) const {
    if (!node[key]) {
        throw std::runtime_error("Missing required configuration: " + yamlPath + "." + key);
    }
    try {
        return node[key].as<T>();
    } catch (const YAML::Exception& e) {
        throw std::runtime_error("Invalid configuration value at " + yamlPath + "." + key + ": " + e.what());
    }
}

// Explicit template instantiations
template std::string ConfigManager::getValueOrThrow<std::string>(const YAML::Node& node, const std::string& key, const std::string& yamlPath) const;
template bool ConfigManager::getValueOrThrow<bool>(const YAML::Node& node, const std::string& key, const std::string& yamlPath) const;
template int ConfigManager::getValueOrThrow<int>(const YAML::Node& node, const std::string& key, const std::string& yamlPath) const;

} // namespace flapi
