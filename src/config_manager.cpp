#include "config_manager.hpp"
#include "endpoint_config_parser.hpp"
#include "config_loader.hpp"
#include "endpoint_repository.hpp"
#include "config_validator.hpp"
#include "config_serializer.hpp"
#include "caching_file_provider.hpp"
#include "vfs_adapter.hpp"
#include <stdexcept>
#include <filesystem>
#include <yaml-cpp/yaml.h>
#include <fstream>
#include <sstream>
#include <iostream>
#include <crow.h>
#include <iomanip>
#include <crow/logging.h>

namespace flapi {

std::chrono::seconds CacheConfig::getRefreshTimeInSeconds() const {
    if (!schedule || schedule->empty()) {
        return std::chrono::seconds::zero();
    }

    auto interval = TimeInterval::parseInterval(*schedule);
    if (!interval) {
        throw std::runtime_error("Invalid cache schedule format: " + *schedule + ". Expected <number>[s|m|h|d]");
    }
    return *interval;
}

// EndpointConfig method implementations
bool EndpointConfig::matchesPath(const std::string& path) const {
    if (isRESTEndpoint()) {
        std::vector<std::string> param_names;
        std::map<std::string, std::string> path_params;
        return RouteTranslator::matchAndExtractParams(urlPath, path, param_names, path_params);
    }
    
    // For MCP endpoints, do simple name comparison
    return getName() == path;
}

ConfigManager::ConfigManager(const std::filesystem::path& config_file)
    : config_file(config_file),
      auth_enabled(false),
      yaml_parser(),
      config_loader(std::make_unique<ConfigLoader>(config_file.string())),
      endpoint_repository(std::make_unique<EndpointRepository>()),
      config_validator(std::make_unique<ConfigValidator>()),
      config_serializer(std::make_unique<ConfigSerializer>())
{}

// Destructor defined here to handle unique_ptr cleanup with complete types
ConfigManager::~ConfigManager() = default;

// Move constructor - defined here with complete types
ConfigManager::ConfigManager(ConfigManager&&) noexcept = default;

// Move assignment operator - defined here with complete types
ConfigManager& ConfigManager::operator=(ConfigManager&&) noexcept = default;

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

        project_name = safeGet<std::string>(config, "project-name", "project-name");
        project_description = safeGet<std::string>(config, "project-description", "project-description");
        server_name = safeGet<std::string>(config, "server-name", "server-name", "localhost");
        http_port = safeGet<int>(config, "http-port", "http-port", 8080);

        CROW_LOG_DEBUG << "Project Name: " << project_name;
        CROW_LOG_DEBUG << "Server Name: " << server_name;
        CROW_LOG_DEBUG << "HTTP Port: " << http_port;

        parseHttpsConfig();
        parseConnections();
        parseRateLimitConfig();
        parseAuthConfig();
        parseDuckDBConfig();
        parseDuckLakeConfig();
        parseMCPConfig();
        parseStorageConfig();
        parseTemplateConfig();
        parseGlobalHeartbeatConfig();

        if (config["cache-schema"]) {
            cache_schema = safeGet<std::string>(config, "cache-schema", "cache-schema");
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

// DuckLake configuration methods
void ConfigManager::parseDuckLakeConfig() {
    ducklake_config = DuckLakeConfig{};

    if (!config["ducklake"]) {
        return;
    }

    auto node = config["ducklake"];
    ducklake_config.enabled = safeGet<bool>(node, "enabled", "ducklake.enabled", false);
    ducklake_config.alias = safeGet<std::string>(node, "alias", "ducklake.alias", std::string("cache"));

    if (ducklake_config.enabled) {
        ducklake_config.metadata_path = safeGet<std::string>(node, "metadata-path", "ducklake.metadata-path");
        ducklake_config.data_path = safeGet<std::string>(node, "data-path", "ducklake.data-path");

        if (node["retention"]) {
            auto retention = node["retention"];
            if (retention["keep-last-snapshots"]) {
                ducklake_config.retention.keep_last_snapshots = safeGet<std::size_t>(retention, "keep-last-snapshots", "ducklake.retention.keep-last-snapshots");
            }
            if (retention["max-snapshot-age"]) {
                ducklake_config.retention.max_snapshot_age = safeGet<std::string>(retention, "max-snapshot-age", "ducklake.retention.max-snapshot-age");
            }
        }

        if (node["compaction"]) {
            auto compaction = node["compaction"];
            ducklake_config.compaction.enabled = safeGet<bool>(compaction, "enabled", "ducklake.compaction.enabled", false);
            if (compaction["schedule"]) {
                ducklake_config.compaction.schedule = safeGet<std::string>(compaction, "schedule", "ducklake.compaction.schedule");
            }
        }

        if (node["scheduler"]) {
            auto scheduler = node["scheduler"];
            ducklake_config.scheduler.enabled = safeGet<bool>(scheduler, "enabled", "ducklake.scheduler.enabled", false);
            if (scheduler["scan-interval"]) {
                ducklake_config.scheduler.scan_interval = safeGet<std::string>(scheduler, "scan-interval", "ducklake.scheduler.scan-interval");
            }
        }

        // Parse data inlining configuration
        if (node["data-inlining-row-limit"]) {
            ducklake_config.data_inlining_row_limit = safeGet<std::size_t>(node, "data-inlining-row-limit", "ducklake.data-inlining-row-limit");
        }

        // Resolve relative paths against base path
        ducklake_config.metadata_path = makePathRelativeToBasePathIfNecessary(ducklake_config.metadata_path);
        ducklake_config.data_path = makePathRelativeToBasePathIfNecessary(ducklake_config.data_path);
    }
}

// Storage configuration methods
void ConfigManager::parseStorageConfig() {
    CROW_LOG_INFO << "Parsing storage configuration";
    storage_config = StorageConfig{};  // Reset to defaults

    if (!config["storage"]) {
        CROW_LOG_DEBUG << "Storage configuration not found, using defaults (cache.enabled=true, cache.ttl=300s, cache.max_size_mb=50)";
        return;
    }

    auto storage_node = config["storage"];

    if (storage_node["cache"]) {
        auto cache_node = storage_node["cache"];
        storage_config.cache.enabled = safeGet<bool>(cache_node, "enabled", "storage.cache.enabled", true);

        if (cache_node["ttl"]) {
            int ttl_seconds = safeGet<int>(cache_node, "ttl", "storage.cache.ttl", 300);
            storage_config.cache.ttl = std::chrono::seconds(ttl_seconds);
            CROW_LOG_DEBUG << "Storage cache TTL: " << ttl_seconds << " seconds";
        }

        if (cache_node["max_size_mb"]) {
            int max_mb = safeGet<int>(cache_node, "max_size_mb", "storage.cache.max_size_mb", 50);
            storage_config.cache.max_size_bytes = static_cast<size_t>(max_mb) * 1024UL * 1024UL;
            CROW_LOG_DEBUG << "Storage cache max size: " << max_mb << " MB";
        }

        CROW_LOG_DEBUG << "Storage cache enabled: " << (storage_config.cache.enabled ? "true" : "false");
    }
}

// MCP configuration methods
void ConfigManager::parseMCPConfig() {
    CROW_LOG_INFO << "Parsing MCP configuration";
    mcp_config = MCPConfig{};  // Initialize with defaults

    if (!config["mcp"]) {
        CROW_LOG_DEBUG << "MCP configuration not found, using defaults (enabled=true, auth.enabled=false)";
        return;
    }

    auto mcp = config["mcp"];
    mcp_config.enabled = safeGet<bool>(mcp, "enabled", "mcp.enabled", true);
    mcp_config.port = safeGet<int>(mcp, "port", "mcp.port", 8081);

    CROW_LOG_DEBUG << "MCP Enabled: " << (mcp_config.enabled ? "true" : "false");
    CROW_LOG_DEBUG << "MCP Port: " << mcp_config.port;

    // Parse MCP authentication configuration
    if (mcp["auth"]) {
        auto auth = mcp["auth"];
        mcp_config.auth.enabled = safeGet<bool>(auth, "enabled", "mcp.auth.enabled", false);
        mcp_config.auth.type = safeGet<std::string>(auth, "type", "mcp.auth.type", "bearer");

        CROW_LOG_DEBUG << "MCP Auth Enabled: " << (mcp_config.auth.enabled ? "true" : "false");
        CROW_LOG_DEBUG << "MCP Auth Type: " << mcp_config.auth.type;

        // Parse Basic auth users if configured
        if (mcp_config.auth.type == "basic") {
            if (auth["users"]) {
                auto users = auth["users"];
                for (const auto& user_entry : users) {
                    AuthUser user;
                    user.username = safeGet<std::string>(user_entry, "username", "mcp.auth.users[].username");
                    user.password = safeGet<std::string>(user_entry, "password", "mcp.auth.users[].password");

                    // Parse roles if present
                    if (user_entry["roles"]) {
                        for (const auto& role : user_entry["roles"]) {
                            user.roles.push_back(role.as<std::string>());
                        }
                    }

                    mcp_config.auth.users.push_back(user);
                    CROW_LOG_DEBUG << "MCP Basic Auth User: " << user.username << " with " << user.roles.size() << " roles";
                }
            } else if (mcp_config.auth.enabled) {
                CROW_LOG_WARNING << "MCP auth enabled with basic type but no users configured";
            }
        }

        // Parse JWT-specific configuration if bearer type
        if (mcp_config.auth.type == "bearer") {
            if (auth["jwt-secret"]) {
                mcp_config.auth.jwt_secret = safeGet<std::string>(auth, "jwt-secret", "mcp.auth.jwt-secret");
                CROW_LOG_DEBUG << "MCP JWT Secret configured";
            } else if (mcp_config.auth.enabled) {
                CROW_LOG_WARNING << "MCP auth enabled with bearer type but jwt-secret not configured";
            }

            mcp_config.auth.jwt_issuer = safeGet<std::string>(auth, "jwt-issuer", "mcp.auth.jwt-issuer", "flapi");
            CROW_LOG_DEBUG << "MCP JWT Issuer: " << mcp_config.auth.jwt_issuer;
        }

        // Parse OIDC configuration if present
        if (auth["oidc"]) {
            CROW_LOG_INFO << "Parsing MCP OIDC configuration";
            mcp_config.auth.oidc = parseOIDCConfigNode(auth["oidc"], "MCP ");
            CROW_LOG_INFO << "MCP OIDC configuration parsed successfully";
        }

        // Parse per-method authentication requirements
        if (auth["methods"]) {
            auto methods = auth["methods"];
            for (const auto& kv : methods) {
                std::string method_name = kv.first.as<std::string>();
                bool required = safeGet<bool>(kv.second, "required", "mcp.auth.methods." + method_name + ".required", true);
                mcp_config.auth.methods[method_name] = MCPMethodAuthConfig{required};
                CROW_LOG_DEBUG << "MCP Method Auth: " << method_name << " required=" << (required ? "true" : "false");
            }
        }
    } else if (mcp_config.enabled) {
        CROW_LOG_DEBUG << "MCP authentication not configured, auth is disabled by default";
    }

    // Parse MCP server instructions (inline)
    if (mcp["instructions"]) {
        mcp_config.instructions = safeGet<std::string>(mcp, "instructions", "mcp.instructions");
        CROW_LOG_DEBUG << "Loaded inline MCP instructions ("
                       << mcp_config.instructions.length() << " characters)";
    }

    // Parse MCP instructions file path
    if (mcp["instructions-file"]) {
        mcp_config.instructions_file = safeGet<std::string>(mcp, "instructions-file", "mcp.instructions-file");
        CROW_LOG_DEBUG << "MCP instructions file: " << mcp_config.instructions_file;
    }
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

        // Use EndpointConfigParser for consistent path resolution
        EndpointConfigParser parser(yaml_parser, this);
        auto parse_result = parser.parseFromFile(config_file);
        
        if (!parse_result.success) {
            // Check if it's just not an endpoint file (vs a real error)
            if (parse_result.error_message.find("Not a valid endpoint configuration") != std::string::npos) {
            CROW_LOG_DEBUG << "\t\tSkipping non-endpoint configuration file: " << config_file;
            return; // Skip this file - it's likely a shared config or template
        }
            std::string error_msg = "Parsing failed (no error message provided)";
            if (!parse_result.error_message.empty()) {
                error_msg = parse_result.error_message;
            }
            throw std::runtime_error(error_msg);
        }
        
        const auto& endpoint = parse_result.config;
        
        // Log parsed endpoint details
        CROW_LOG_DEBUG << "\t\t" << endpoint.getShortDescription();
        
        if (endpoint.isMCPPrompt()) {
            CROW_LOG_DEBUG << "\t\tTemplate Content: embedded in config";
            } else {
            CROW_LOG_DEBUG << "\t\tTemplate Source: " << endpoint.templateSource;
        }
        
        // Add to endpoints list
        endpoints.push_back(endpoint);

        // Log configuration summary
        CROW_LOG_DEBUG << "\t\tConfiguration loaded: " << endpoint.getShortDescription();
        
    } catch (const std::exception& e) {
        std::string what_msg = e.what();
        if (what_msg.empty()) {
            what_msg = "(empty exception message)";
        }
        throw std::runtime_error("Error loading endpoint config from file: " + config_file + ", Error: " + what_msg);
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
            aws_config.secret_name = safeGet<std::string>(aws_node, "secret-name", "auth.from-aws-secretmanager.secret-name");
            if (aws_node["region"]) {
                aws_config.region = safeGet<std::string>(aws_node, "region", "auth.from-aws-secretmanager.region");
            }

            if (aws_node["secret-id"]) {
                aws_config.secret_id = safeGet<std::string>(aws_node, "secret-id", "auth.from-aws-secretmanager.secret-id");
            }

            if (aws_node["secret-key"]) {
                aws_config.secret_key = safeGet<std::string>(aws_node, "secret-key", "auth.from-aws-secretmanager.secret-key");
            }

            aws_config.secret_table = safeGet<std::string>(aws_node, "secret-table", 
                                                           "auth.from-aws-secretmanager.secret-table", 
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
    if (!endpoint_config["cache"]) {
        endpoint.cache.enabled = false;
        return;
    }

        auto cache_node = endpoint_config["cache"];

    endpoint.cache.enabled = safeGet<bool>(cache_node, "enabled", "cache.enabled", true);
    endpoint.cache.table = safeGet<std::string>(cache_node, "table", "cache.table");
    endpoint.cache.schema = safeGet<std::string>(cache_node, "schema", "cache.schema", std::string("cache"));

    if (cache_node["schedule"]) {
        endpoint.cache.schedule = safeGet<std::string>(cache_node, "schedule", "cache.schedule");
    }

    if (cache_node["primary-key"]) {
        endpoint.cache.primary_keys = safeGet<std::vector<std::string>>(cache_node, "primary-key", "cache.primary-key");
    } else if (cache_node["primaryKey"]) {
        endpoint.cache.primary_keys = safeGet<std::vector<std::string>>(cache_node, "primaryKey", "cache.primaryKey");
    }

    if (cache_node["cursor"]) {
        auto cursor_node = cache_node["cursor"];
        CacheConfig::CursorConfig cursor;
        cursor.column = safeGet<std::string>(cursor_node, "column", "cache.cursor.column");
        cursor.type = safeGet<std::string>(cursor_node, "type", "cache.cursor.type");
        endpoint.cache.cursor = cursor;
    }

    if (cache_node["rollback-window"]) {
        endpoint.cache.rollback_window = safeGet<std::string>(cache_node, "rollback-window", "cache.rollback-window");
    } else if (cache_node["rollbackWindow"]) {
        endpoint.cache.rollback_window = safeGet<std::string>(cache_node, "rollbackWindow", "cache.rollbackWindow");
    }

    if (cache_node["retention"]) {
        auto retention_node = cache_node["retention"];
        if (retention_node["keep-last-snapshots"]) {
            endpoint.cache.retention.keep_last_snapshots = safeGet<std::size_t>(retention_node, "keep-last-snapshots", "cache.retention.keep-last-snapshots");
        }
        if (retention_node["max-snapshot-age"]) {
            endpoint.cache.retention.max_snapshot_age = safeGet<std::string>(retention_node, "max-snapshot-age", "cache.retention.max-snapshot-age");
        }
    }

    if (cache_node["delete-handling"]) {
        endpoint.cache.delete_handling = safeGet<std::string>(cache_node, "delete-handling", "cache.delete-handling");
    } else if (cache_node["deleteHandling"]) {
        endpoint.cache.delete_handling = safeGet<std::string>(cache_node, "deleteHandling", "cache.deleteHandling");
    }

    if (cache_node["template-file"]) {
        std::string template_file_value = safeGet<std::string>(cache_node, "template-file", "cache.template-file");
        std::filesystem::path template_file_path(template_file_value);
        
        // If absolute path, keep as-is; otherwise resolve relative to endpoint_dir
        if (template_file_path.is_absolute()) {
            endpoint.cache.template_file = template_file_value;
        } else {
            endpoint.cache.template_file = (endpoint_dir / template_file_path).string();
        }
    }

    CROW_LOG_DEBUG << "\t\tCache Enabled: " << (endpoint.cache.enabled ? "true" : "false");
    CROW_LOG_DEBUG << "\t\tCache Table: " << endpoint.cache.table;
    CROW_LOG_DEBUG << "\t\tCache Schema: " << endpoint.cache.schema;
    if (endpoint.cache.schedule) {
        CROW_LOG_DEBUG << "\t\tSchedule: " << endpoint.cache.schedule.value();
    }
    if (endpoint.cache.cursor) {
        CROW_LOG_DEBUG << "\t\tCursor Column: " << endpoint.cache.cursor->column << " Type: " << endpoint.cache.cursor->type;
    }
    if (endpoint.cache.rollback_window) {
        CROW_LOG_DEBUG << "\t\tRollback Window: " << endpoint.cache.rollback_window.value();
    }
    if (!endpoint.cache.primary_keys.empty()) {
        CROW_LOG_DEBUG << "\t\tPrimary Keys: " << endpoint.cache.primary_keys.size();
    }
    if (endpoint.cache.template_file) {
        CROW_LOG_DEBUG << "\t\tTemplate File: " << endpoint.cache.template_file.value();
    }
    
    // Parse write operation cache options
    if (cache_node["invalidate-on-write"]) {
        endpoint.cache.invalidate_on_write = safeGet<bool>(cache_node, "invalidate-on-write", "cache.invalidate-on-write", false);
    }
    
    if (cache_node["refresh-on-write"]) {
        endpoint.cache.refresh_on_write = safeGet<bool>(cache_node, "refresh-on-write", "cache.refresh-on-write", false);
    }
    
    if (endpoint.cache.invalidate_on_write) {
        CROW_LOG_DEBUG << "\t\tInvalidate on Write: true";
    }
    if (endpoint.cache.refresh_on_write) {
        CROW_LOG_DEBUG << "\t\tRefresh on Write: true";
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
            auth_config.jwt_secret = auth_node["jwt-secret"].as<std::string>();
            auth_config.jwt_issuer = auth_node["jwt-issuer"].as<std::string>();
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

            // Parse OIDC configuration if present
            if (auth_node["oidc"]) {
                CROW_LOG_INFO << "Parsing OIDC configuration";
                auth_config.oidc = parseOIDCConfigNode(auth_node["oidc"]);
                CROW_LOG_INFO << "OIDC configuration parsed successfully";
            }
        }
    }
}

// Helper to parse OIDC configuration from a YAML node
OIDCConfig ConfigManager::parseOIDCConfigNode(const YAML::Node& oidc_node, const std::string& log_prefix) const {
    OIDCConfig oidc_config;

    // Basic OIDC settings
    if (oidc_node["issuer-url"]) {
        oidc_config.issuer_url = oidc_node["issuer-url"].as<std::string>();
        CROW_LOG_DEBUG << log_prefix << "OIDC issuer URL: " << oidc_config.issuer_url;
    }

    if (oidc_node["client-id"]) {
        oidc_config.client_id = oidc_node["client-id"].as<std::string>();
        CROW_LOG_DEBUG << log_prefix << "OIDC client ID: " << oidc_config.client_id;
    }

    if (oidc_node["client-secret"]) {
        oidc_config.client_secret = oidc_node["client-secret"].as<std::string>();
    }

    // Provider type for presets
    if (oidc_node["provider-type"]) {
        oidc_config.provider_type = oidc_node["provider-type"].as<std::string>();
        CROW_LOG_DEBUG << log_prefix << "OIDC provider type: " << oidc_config.provider_type;
    }

    // Token validation settings
    if (oidc_node["allowed-audiences"]) {
        oidc_config.allowed_audiences = oidc_node["allowed-audiences"].as<std::vector<std::string>>();
    }

    if (oidc_node["verify-expiration"]) {
        oidc_config.verify_expiration = oidc_node["verify-expiration"].as<bool>();
    }

    if (oidc_node["clock-skew-seconds"]) {
        oidc_config.clock_skew_seconds = oidc_node["clock-skew-seconds"].as<int>();
    }

    // Claim mapping
    if (oidc_node["username-claim"]) {
        oidc_config.username_claim = oidc_node["username-claim"].as<std::string>();
    }

    if (oidc_node["email-claim"]) {
        oidc_config.email_claim = oidc_node["email-claim"].as<std::string>();
    }

    if (oidc_node["roles-claim"]) {
        oidc_config.roles_claim = oidc_node["roles-claim"].as<std::string>();
    }

    if (oidc_node["groups-claim"]) {
        oidc_config.groups_claim = oidc_node["groups-claim"].as<std::string>();
    }

    if (oidc_node["role-claim-path"]) {
        oidc_config.role_claim_path = oidc_node["role-claim-path"].as<std::string>();
        CROW_LOG_DEBUG << log_prefix << "OIDC role claim path: " << oidc_config.role_claim_path;
    }

    // OAuth flows
    if (oidc_node["enable-client-credentials"]) {
        oidc_config.enable_client_credentials = oidc_node["enable-client-credentials"].as<bool>();
    }

    if (oidc_node["enable-refresh-tokens"]) {
        oidc_config.enable_refresh_tokens = oidc_node["enable-refresh-tokens"].as<bool>();
    }

    if (oidc_node["scopes"]) {
        oidc_config.scopes = oidc_node["scopes"].as<std::vector<std::string>>();
    }

    // JWKS caching
    if (oidc_node["jwks-cache-hours"]) {
        oidc_config.jwks_cache_hours = oidc_node["jwks-cache-hours"].as<int>();
    }

    return oidc_config;
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
    if (!endpoint.cache.enabled || endpoint.cache.table.empty()) {
        return "";
    }
    // Cache templates now resolved via YAML; this helper is deprecated in DuckLake flow
    return "";
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
std::filesystem::path ConfigManager::getFullTemplatePath() const { return std::filesystem::path(base_path) / template_config.path; }
std::shared_ptr<IFileProvider> ConfigManager::getFileProvider() const {
    // For remote template paths with caching enabled, wrap with CachingFileProvider
    if (storage_config.cache.enabled && PathSchemeUtils::IsRemotePath(template_config.path)) {
        FileCacheConfig cache_config;
        cache_config.enabled = true;
        cache_config.ttl = storage_config.cache.ttl;
        cache_config.max_size_bytes = storage_config.cache.max_size_bytes;

        auto base_provider = FileProviderFactory::CreateDuckDBProvider();
        return std::make_shared<CachingFileProvider>(base_provider, cache_config);
    }

    return config_loader->getFileProvider();
}
std::string ConfigManager::getCacheSchema() const { return cache_schema; }
const std::unordered_map<std::string, ConnectionConfig>& ConfigManager::getConnections() const { return connections; }
const RateLimitConfig& ConfigManager::getRateLimitConfig() const { return rate_limit_config; }
bool ConfigManager::isHttpsEnforced() const { return https_config.enabled; }
const HttpsConfig& ConfigManager::getHttpsConfig() const { return https_config; }
bool ConfigManager::isAuthEnabled() const { return auth_enabled; }
const std::vector<EndpointConfig>& ConfigManager::getEndpoints() const { return endpoints; }
std::string ConfigManager::getBasePath() const { return base_path.string(); }

std::string ConfigManager::loadMCPInstructions() const {
    // Priority 1: Inline instructions (if provided)
    if (!mcp_config.instructions.empty()) {
        return mcp_config.instructions;
    }

    // Priority 2: Load from file (if provided)
    if (!mcp_config.instructions_file.empty()) {
        std::string file_path = mcp_config.instructions_file;

        // Make path absolute if relative
        if (file_path[0] != '/') {
            file_path = (std::filesystem::path(base_path) / file_path).string();
        }

        // Read file content
        std::ifstream file(file_path);
        if (!file.is_open()) {
            CROW_LOG_WARNING << "Failed to open MCP instructions file: " << file_path;
            return "";
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string content = buffer.str();

        CROW_LOG_DEBUG << "Loaded MCP instructions from file ("
                       << content.length() << " characters): " << file_path;

        return content;
    }

    // No instructions configured
    return "";
}

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

    // Add DuckLake configuration
    crow::json::wvalue ducklakeJson;
    ducklakeJson["enabled"] = ducklake_config.enabled;
    ducklakeJson["alias"] = ducklake_config.alias;
    ducklakeJson["metadata-path"] = ducklake_config.metadata_path;
    ducklakeJson["data-path"] = ducklake_config.data_path;

    // Add data inlining configuration
    if (ducklake_config.data_inlining_row_limit) {
        ducklakeJson["data-inlining-row-limit"] = static_cast<int64_t>(ducklake_config.data_inlining_row_limit.value());
    }

    // Add retention configuration
    if (ducklake_config.retention.keep_last_snapshots || ducklake_config.retention.max_snapshot_age) {
        crow::json::wvalue retentionJson;
        if (ducklake_config.retention.keep_last_snapshots) {
            retentionJson["keep-last-snapshots"] = static_cast<int64_t>(ducklake_config.retention.keep_last_snapshots.value());
        }
        if (ducklake_config.retention.max_snapshot_age) {
            retentionJson["max-snapshot-age"] = ducklake_config.retention.max_snapshot_age.value();
        }
        ducklakeJson["retention"] = std::move(retentionJson);
    }

    // Add compaction configuration
    if (ducklake_config.compaction.enabled || ducklake_config.compaction.schedule) {
        crow::json::wvalue compactionJson;
        compactionJson["enabled"] = ducklake_config.compaction.enabled;
        if (ducklake_config.compaction.schedule) {
            compactionJson["schedule"] = ducklake_config.compaction.schedule.value();
        }
        ducklakeJson["compaction"] = std::move(compactionJson);
    }

    // Add scheduler configuration
    if (ducklake_config.scheduler.enabled || ducklake_config.scheduler.scan_interval) {
        crow::json::wvalue schedulerJson;
        schedulerJson["enabled"] = ducklake_config.scheduler.enabled;
        if (ducklake_config.scheduler.scan_interval) {
            schedulerJson["scan-interval"] = ducklake_config.scheduler.scan_interval.value();
        }
        ducklakeJson["scheduler"] = std::move(schedulerJson);
    }

    result["ducklake"] = std::move(ducklakeJson);
    
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
        awsJson[(style == EndpointJsonStyle::HyphenCase) ? "secret-name" : "secretName"] = config.auth.from_aws_secretmanager->secret_name;
        awsJson[(style == EndpointJsonStyle::HyphenCase) ? "region" : "region"] = config.auth.from_aws_secretmanager->region;
        authJson[(style == EndpointJsonStyle::HyphenCase) ? "from-aws-secretmanager" : "fromAwsSecretmanager"] = std::move(awsJson);
    }
    json["auth"] = std::move(authJson);

    // Always include cache section
    crow::json::wvalue cacheJson;
    cacheJson["enabled"] = config.cache.enabled;
    cacheJson[(style == EndpointJsonStyle::HyphenCase) ? "table" : "table"] = config.cache.table;
    cacheJson[(style == EndpointJsonStyle::HyphenCase) ? "schema" : "schema"] = config.cache.schema;
    if (config.cache.schedule) {
        cacheJson[(style == EndpointJsonStyle::HyphenCase) ? "schedule" : "schedule"] = config.cache.schedule.value();
    }
    if (!config.cache.primary_keys.empty()) {
        auto pk_list = crow::json::wvalue::list();
        for (const auto& pk : config.cache.primary_keys) {
            pk_list.push_back(pk);
        }
        cacheJson[(style == EndpointJsonStyle::HyphenCase) ? "primary-key" : "primaryKey"] = std::move(pk_list);
    }
    if (config.cache.cursor) {
        crow::json::wvalue cursorJson;
        cursorJson[(style == EndpointJsonStyle::HyphenCase) ? "column" : "column"] = config.cache.cursor->column;
        cursorJson[(style == EndpointJsonStyle::HyphenCase) ? "type" : "type"] = config.cache.cursor->type;
        cacheJson[(style == EndpointJsonStyle::HyphenCase) ? "cursor" : "cursor"] = std::move(cursorJson);
    }
    if (config.cache.rollback_window) {
        cacheJson[(style == EndpointJsonStyle::HyphenCase) ? "rollback-window" : "rollbackWindow"] = config.cache.rollback_window.value();
    }
    if (config.cache.retention.keep_last_snapshots || config.cache.retention.max_snapshot_age) {
        crow::json::wvalue retentionJson;
        if (config.cache.retention.keep_last_snapshots) {
            retentionJson[(style == EndpointJsonStyle::HyphenCase) ? "keep-last-snapshots" : "keepLastSnapshots"] = static_cast<int64_t>(config.cache.retention.keep_last_snapshots.value());
        }
        if (config.cache.retention.max_snapshot_age) {
            retentionJson[(style == EndpointJsonStyle::HyphenCase) ? "max-snapshot-age" : "maxSnapshotAge"] = config.cache.retention.max_snapshot_age.value();
        }
        cacheJson[(style == EndpointJsonStyle::HyphenCase) ? "retention" : "retention"] = std::move(retentionJson);
    }
    if (config.cache.delete_handling) {
        cacheJson[(style == EndpointJsonStyle::HyphenCase) ? "delete-handling" : "deleteHandling"] = config.cache.delete_handling.value();
    }
    if (config.cache.template_file) {
        cacheJson[(style == EndpointJsonStyle::HyphenCase) ? "template-file" : "templateFile"] = config.cache.template_file.value();
    }
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
        config.cache.enabled = getBool({"enabled"}, true);

        auto tableKey = firstExistingKey(cacheJson, {"table"});
        if (!tableKey.empty()) {
            config.cache.table = cacheJson[tableKey].s();
        }
        auto schemaKey = firstExistingKey(cacheJson, {"schema"});
        if (!schemaKey.empty()) {
            config.cache.schema = cacheJson[schemaKey].s();
        }
        auto scheduleKey = firstExistingKey(cacheJson, {"schedule"});
        if (!scheduleKey.empty()) {
            config.cache.schedule = cacheJson[scheduleKey].s();
        }
        auto pkKey = firstExistingKey(cacheJson, {"primary-key", "primaryKey"});
        if (!pkKey.empty()) {
            std::vector<std::string> pks;
            for (const auto& item : cacheJson[pkKey]) {
                pks.push_back(item.s());
            }
            config.cache.primary_keys = std::move(pks);
        }
        auto cursorKey = firstExistingKey(cacheJson, {"cursor"});
        if (!cursorKey.empty()) {
            CacheConfig::CursorConfig cursor;
            const auto& cursorJson = cacheJson[cursorKey];
            cursor.column = cursorJson[requireStringField(cursorJson, {"column"})].s();
            cursor.type = cursorJson[requireStringField(cursorJson, {"type"})].s();
            config.cache.cursor = std::move(cursor);
        }
        auto rollbackKey = firstExistingKey(cacheJson, {"rollback-window", "rollbackWindow"});
        if (!rollbackKey.empty()) {
            config.cache.rollback_window = cacheJson[rollbackKey].s();
        }
        if (cacheJson.has("retention")) {
            const auto& retentionJson = cacheJson["retention"];
            auto keepKey = firstExistingKey(retentionJson, {"keep-last-snapshots", "keepLastSnapshots"});
            if (!keepKey.empty()) {
                config.cache.retention.keep_last_snapshots = static_cast<std::size_t>(retentionJson[keepKey].i());
            }
            auto ageKey = firstExistingKey(retentionJson, {"max-snapshot-age", "maxSnapshotAge"});
            if (!ageKey.empty()) {
                config.cache.retention.max_snapshot_age = retentionJson[ageKey].s();
            }
        }
        auto deleteKey = firstExistingKey(cacheJson, {"delete-handling", "deleteHandling"});
        if (!deleteKey.empty()) {
            config.cache.delete_handling = cacheJson[deleteKey].s();
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
    // Also add to repository for unified access
    if (endpoint_repository) {
        endpoint_repository->addEndpoint(endpoint);
    }
}

bool ConfigManager::removeEndpointByPath(const std::string& path) {
    auto before = endpoints.size();
    endpoints.erase(
        std::remove_if(endpoints.begin(), endpoints.end(), [&](const EndpointConfig& endpoint) {
            return endpoint.matchesPath(path);
        }),
        endpoints.end());

    // Also remove from repository
    if (endpoint_repository && before != endpoints.size()) {
        if (auto endpoint = getEndpointForPath(path)) {
            if (endpoint->isRESTEndpoint()) {
                endpoint_repository->removeRestEndpoint(endpoint->urlPath, endpoint->method);
            } else if (endpoint->isMCPTool()) {
                endpoint_repository->removeMCPEndpoint(endpoint->mcp_tool->name);
            } else if (endpoint->isMCPResource()) {
                endpoint_repository->removeMCPEndpoint(endpoint->mcp_resource->name);
            } else if (endpoint->isMCPPrompt()) {
                endpoint_repository->removeMCPEndpoint(endpoint->mcp_prompt->name);
            }
        }
    }

    return before != endpoints.size();
}

bool ConfigManager::replaceEndpoint(const EndpointConfig& endpoint) {
    for (auto& candidate : endpoints) {
        if (endpoint.isSameEndpoint(candidate)) {
            candidate = endpoint;

            // Also update in repository
            if (endpoint_repository) {
                if (endpoint.isRESTEndpoint()) {
                    endpoint_repository->removeRestEndpoint(endpoint.urlPath, endpoint.method);
                    endpoint_repository->addEndpoint(endpoint);
                } else if (endpoint.isMCPTool()) {
                    endpoint_repository->removeMCPEndpoint(endpoint.mcp_tool->name);
                    endpoint_repository->addEndpoint(endpoint);
                } else if (endpoint.isMCPResource()) {
                    endpoint_repository->removeMCPEndpoint(endpoint.mcp_resource->name);
                    endpoint_repository->addEndpoint(endpoint);
                } else if (endpoint.isMCPPrompt()) {
                    endpoint_repository->removeMCPEndpoint(endpoint.mcp_prompt->name);
                    endpoint_repository->addEndpoint(endpoint);
                }
            }

            return true;
        }
    }
    return false;
}

const EndpointConfig* ConfigManager::getEndpointForPath(const std::string& path) const {
    // First try to find exact match (no method filtering)
    for (const auto& endpoint : endpoints) {
        if (endpoint.matchesPath(path)) {
            return &endpoint;
        }
    }
    return nullptr;
}

const EndpointConfig* ConfigManager::getEndpointForPathAndMethod(const std::string& path, const std::string& httpMethod) const {
    std::string methodUpper = httpMethod;
    std::transform(methodUpper.begin(), methodUpper.end(), methodUpper.begin(), ::toupper);
    
    for (const auto& endpoint : endpoints) {
        if (!endpoint.matchesPath(path)) {
            continue;
        }
        
        // Match HTTP method
        std::string endpointMethod = endpoint.method.empty() ? "GET" : endpoint.method;
        std::transform(endpointMethod.begin(), endpointMethod.end(), endpointMethod.begin(), ::toupper);
        
        if (endpointMethod == methodUpper) {
            return &endpoint;
        }
    }
    return nullptr;
}



// YAML Serialization
std::string ConfigManager::serializeEndpointConfigToYaml(const EndpointConfig& config) const {
    YAML::Emitter out;
    out << YAML::BeginMap;
    
    // REST endpoint fields
    if (config.isRESTEndpoint()) {
        out << YAML::Key << "url-path" << YAML::Value << config.urlPath;
        out << YAML::Key << "method" << YAML::Value << config.method;
    }
    
    // MCP Tool fields
    if (config.isMCPTool()) {
        out << YAML::Key << "mcp-tool" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "name" << YAML::Value << config.mcp_tool->name;
        if (!config.mcp_tool->description.empty()) {
            out << YAML::Key << "description" << YAML::Value << config.mcp_tool->description;
        }
        if (!config.mcp_tool->result_mime_type.empty()) {
            out << YAML::Key << "result-mime-type" << YAML::Value << config.mcp_tool->result_mime_type;
        }
        out << YAML::EndMap;
    }
    
    // MCP Resource fields
    if (config.isMCPResource()) {
        out << YAML::Key << "mcp-resource" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "name" << YAML::Value << config.mcp_resource->name;
        if (!config.mcp_resource->description.empty()) {
            out << YAML::Key << "description" << YAML::Value << config.mcp_resource->description;
        }
        if (!config.mcp_resource->mime_type.empty()) {
            out << YAML::Key << "mime-type" << YAML::Value << config.mcp_resource->mime_type;
        }
        out << YAML::EndMap;
    }
    
    // MCP Prompt fields
    if (config.isMCPPrompt()) {
        out << YAML::Key << "mcp-prompt" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "name" << YAML::Value << config.mcp_prompt->name;
        if (!config.mcp_prompt->description.empty()) {
            out << YAML::Key << "description" << YAML::Value << config.mcp_prompt->description;
        }
        out << YAML::EndMap;
    }
    
    // Common fields
    out << YAML::Key << "template-source" << YAML::Value << config.templateSource;
    
    if (!config.connection.empty()) {
        out << YAML::Key << "connection" << YAML::Value << YAML::BeginSeq;
        for (const auto& conn : config.connection) {
            out << conn;
        }
        out << YAML::EndSeq;
    }
    
    // Request fields
    if (!config.request_fields.empty()) {
        out << YAML::Key << "request" << YAML::Value << YAML::BeginSeq;
        for (const auto& field : config.request_fields) {
            out << YAML::BeginMap;
            out << YAML::Key << "field-name" << YAML::Value << field.fieldName;
            out << YAML::Key << "field-in" << YAML::Value << field.fieldIn;
            if (!field.description.empty()) {
                out << YAML::Key << "description" << YAML::Value << field.description;
            }
            out << YAML::Key << "required" << YAML::Value << field.required;
            out << YAML::EndMap;
        }
        out << YAML::EndSeq;
    }
    
    // Auth configuration
    if (config.auth.enabled) {
        out << YAML::Key << "auth" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "enabled" << YAML::Value << true;
        out << YAML::Key << "type" << YAML::Value << config.auth.type;
        out << YAML::EndMap;
    }
    
    // Cache configuration
    if (config.cache.enabled) {
        out << YAML::Key << "cache" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "enabled" << YAML::Value << true;
        out << YAML::Key << "table" << YAML::Value << config.cache.table;
        out << YAML::Key << "schema" << YAML::Value << config.cache.schema;
        if (config.cache.template_file) {
            out << YAML::Key << "template-file" << YAML::Value << config.cache.template_file.value();
        }
        out << YAML::EndMap;
    }
    
    out << YAML::EndMap;
    return out.c_str();
}

// YAML Deserialization
EndpointConfig ConfigManager::deserializeEndpointConfigFromYaml(const std::string& yaml_content) const {
    YAML::Node node = YAML::Load(yaml_content);
    
    EndpointConfig config;
    
    // Parse using existing loadEndpointConfig logic
    // Reuse the parsing code from loadEndpointConfig
    bool is_rest_endpoint = node["url-path"].IsDefined();
    bool is_mcp_tool = node["mcp-tool"].IsDefined();
    bool is_mcp_resource = node["mcp-resource"].IsDefined();
    bool is_mcp_prompt = node["mcp-prompt"].IsDefined();
    
    if (!is_rest_endpoint && !is_mcp_tool && !is_mcp_resource && !is_mcp_prompt) {
        throw std::runtime_error("Invalid endpoint configuration: must define url-path, mcp-tool, mcp-resource, or mcp-prompt");
    }
    
    // Parse REST endpoint
    if (is_rest_endpoint) {
        config.urlPath = node["url-path"].as<std::string>();
        config.method = node["method"] ? node["method"].as<std::string>() : "GET";
    }
    
    // Parse MCP Tool
    if (is_mcp_tool) {
        EndpointConfig::MCPToolInfo tool;
        tool.name = node["mcp-tool"]["name"].as<std::string>();
        tool.description = node["mcp-tool"]["description"] ? node["mcp-tool"]["description"].as<std::string>() : "";
        tool.result_mime_type = node["mcp-tool"]["result-mime-type"] ? node["mcp-tool"]["result-mime-type"].as<std::string>() : "application/json";
        config.mcp_tool = tool;
    }
    
    // Parse MCP Resource
    if (is_mcp_resource) {
        EndpointConfig::MCPResourceInfo resource;
        resource.name = node["mcp-resource"]["name"].as<std::string>();
        resource.description = node["mcp-resource"]["description"] ? node["mcp-resource"]["description"].as<std::string>() : "";
        resource.mime_type = node["mcp-resource"]["mime-type"] ? node["mcp-resource"]["mime-type"].as<std::string>() : "text/plain";
        config.mcp_resource = resource;
    }
    
    // Parse MCP Prompt
    if (is_mcp_prompt) {
        EndpointConfig::MCPPromptInfo mcp_prompt_config;
        mcp_prompt_config.name = node["mcp-prompt"]["name"].as<std::string>();
        mcp_prompt_config.description = node["mcp-prompt"]["description"] ? node["mcp-prompt"]["description"].as<std::string>() : "";
        config.mcp_prompt = mcp_prompt_config;
    }
    
    // Common fields
    config.templateSource = node["template-source"].as<std::string>();
    
    if (node["connection"]) {
        for (const auto& conn : node["connection"]) {
            config.connection.push_back(conn.as<std::string>());
        }
    }
    
    // Parse cache config
    if (node["cache"] && node["cache"]["enabled"].as<bool>(false)) {
        config.cache.enabled = true;
        config.cache.table = node["cache"]["table"].as<std::string>();
        config.cache.schema = node["cache"]["schema"].as<std::string>("cache");
        if (node["cache"]["template-file"]) {
            config.cache.template_file = node["cache"]["template-file"].as<std::string>();
        }
    }
    
    return config;
}

// Validation
ConfigManager::ValidationResult ConfigManager::validateEndpointConfig(const EndpointConfig& config) const {
    ValidationResult result;
    result.valid = true;
    
    // Use endpoint's self-validation for type-specific checks
    auto self_errors = config.validateSelf();
    if (!self_errors.empty()) {
        result.valid = false;
        result.errors.insert(result.errors.end(), self_errors.begin(), self_errors.end());
    }
    
    // Validate template source
    if (config.templateSource.empty()) {
        result.valid = false;
        result.errors.emplace_back("template-source cannot be empty");
    } else {
        // Check if template file exists
        // Note: config.templateSource is already resolved relative to YAML file directory during parsing
        std::filesystem::path template_path(config.templateSource);
        
        // If path is not absolute, prepend template_config.path
        if (!template_path.is_absolute()) {
            template_path = std::filesystem::path(template_config.path) / template_path;
        }
        
        if (!std::filesystem::exists(template_path)) {
            result.warnings.emplace_back("Template file does not exist: " + template_path.string());
        }
    }
    
    // Validate connections
    if (config.connection.empty()) {
        result.warnings.emplace_back("No database connection specified");
    } else {
        for (const auto& conn_name : config.connection) {
            if (connections.find(conn_name) == connections.end()) {
                result.valid = false;
                result.errors.emplace_back("Connection '" + conn_name + "' not found in configuration");
            }
        }
    }
    
    // Validate cache template if specified
    if (config.cache.enabled && config.cache.template_file) {
        // Note: config.cache.template_file is already resolved relative to YAML file directory during parsing
        std::filesystem::path cache_template_path(config.cache.template_file.value());
        
        // If path is not absolute, prepend template_config.path
        if (!cache_template_path.is_absolute()) {
            cache_template_path = std::filesystem::path(template_config.path) / cache_template_path;
        }
        
        if (!std::filesystem::exists(cache_template_path)) {
            result.warnings.emplace_back("Cache template file does not exist: " + cache_template_path.string());
        }
    }
    
    return result;
}

// Persistence
void ConfigManager::persistEndpointConfigToFile(const EndpointConfig& config, const std::filesystem::path& file_path) const {
    std::string yaml_content = serializeEndpointConfigToYaml(config);
    
    // Ensure parent directory exists
    if (file_path.has_parent_path()) {
        std::filesystem::create_directories(file_path.parent_path());
    }
    
    // Write to file
    std::ofstream file(file_path);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open file for writing: " + file_path.string());
    }
    
    file << yaml_content;
    if (!file.good()) {
        throw std::runtime_error("Failed to write to file: " + file_path.string());
    }
    
    CROW_LOG_INFO << "Persisted endpoint configuration to: " << file_path;
}

// Validate from YAML string (does not modify files)
ConfigManager::ValidationResult ConfigManager::validateEndpointConfigFromYaml(const std::string& yaml_content) const {
    try {
        EndpointConfig config = deserializeEndpointConfigFromYaml(yaml_content);
        return validateEndpointConfig(config);
    } catch (const std::exception& e) {
        ValidationResult result;
        result.valid = false;
        result.errors.emplace_back(std::string("YAML parsing error: ") + e.what());
        return result;
    }
}

// Validate from file (reads but does not modify)
ConfigManager::ValidationResult ConfigManager::validateEndpointConfigFile(const std::filesystem::path& file_path) const {
    ValidationResult result;
    
    // Check if file exists
    if (!std::filesystem::exists(file_path)) {
        result.valid = false;
        result.errors.emplace_back("File does not exist: " + file_path.string());
        return result;
    }
    
    try {
        // Use EndpointConfigParser for consistent path resolution
        // Note: We const_cast here because parsing doesn't logically modify ConfigManager state
        auto* mutable_this = const_cast<ConfigManager*>(this);
        EndpointConfigParser parser(mutable_this->yaml_parser, mutable_this);
        auto parse_result = parser.parseFromFile(file_path);
        
        if (!parse_result.success) {
            result.valid = false;
            result.errors.emplace_back(parse_result.error_message);
            return result;
        }
        
        // Now validate the properly resolved configuration
        return validateEndpointConfig(parse_result.config);
        
    } catch (const std::exception& e) {
        result.valid = false;
        result.errors.emplace_back(std::string("Validation error: ") + e.what());
        return result;
    }
}

// Reload endpoint from disk (after external edit)
// Note: This method modifies the endpoints vector. Ensure proper synchronization if called from multiple threads.
bool ConfigManager::reloadEndpointConfig(const std::string& slug_or_path) {
    // Try to find existing endpoint by URL path or MCP name
    auto it = std::find_if(endpoints.begin(), endpoints.end(), [&](const EndpointConfig& ep) {
        return ep.getName() == slug_or_path || ep.matchesPath(slug_or_path);
    });
    
    if (it == endpoints.end()) {
        CROW_LOG_WARNING << "Endpoint not found for reload: " << slug_or_path;
        return false;
    }
    
    // Get the YAML configuration file path
    std::filesystem::path yaml_file;
    if (!it->config_file_path.empty()) {
        // Use the stored config file path (set during initial load by EndpointConfigParser)
        yaml_file = it->config_file_path;
    } else {
        // Fallback for endpoints loaded before the refactoring (derive from templateSource)
        yaml_file = std::filesystem::path(template_config.path) / it->templateSource;
        
        // If templateSource is a .sql file, look for the corresponding .yaml file
        if (yaml_file.extension() == ".sql") {
            auto yaml_candidate = yaml_file;
            yaml_candidate.replace_extension(".yaml");
            if (std::filesystem::exists(yaml_candidate)) {
                yaml_file = yaml_candidate;
            } else {
                yaml_candidate = yaml_file;
                yaml_candidate.replace_extension(".yml");
                if (std::filesystem::exists(yaml_candidate)) {
                    yaml_file = yaml_candidate;
                } else {
                    // Neither .yaml nor .yml exists, default to .yaml
                    yaml_file.replace_extension(".yaml");
                }
            }
        }
    }
    
    if (!std::filesystem::exists(yaml_file)) {
        CROW_LOG_ERROR << "YAML file not found for reload: " << yaml_file;
        return false;
    }
    
    // Validate the file first
    ValidationResult validation = validateEndpointConfigFile(yaml_file);
    if (!validation.valid) {
        CROW_LOG_ERROR << "Validation failed for " << yaml_file << ":";
        for (const auto& error : validation.errors) {
            CROW_LOG_ERROR << "  - " << error;
        }
        return false;
    }
    
    try {
        // Use EndpointConfigParser for consistent path resolution
        EndpointConfigParser parser(yaml_parser, this);
        auto parse_result = parser.parseFromFile(yaml_file);
        
        if (!parse_result.success) {
            CROW_LOG_ERROR << "Failed to parse endpoint config: " << parse_result.error_message;
            return false;
        }
        
        // Replace the existing endpoint with the reloaded configuration
        *it = parse_result.config;
        
        CROW_LOG_INFO << "Reloaded endpoint configuration from: " << yaml_file;
        
        // Log warnings if any (from both validation and parsing)
        for (const auto& warning : validation.warnings) {
            CROW_LOG_WARNING << "  - " << warning;
        }
        for (const auto& warning : parse_result.warnings) {
            CROW_LOG_WARNING << "  - " << warning;
        }
        
        return true;
    } catch (const std::exception& e) {
        CROW_LOG_ERROR << "Failed to reload endpoint: " << e.what();
        return false;
    }
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

// Explicit template instantiations for safeGet (required by EndpointConfigParser)
template std::string ConfigManager::safeGet<std::string>(const YAML::Node& node, const std::string& key, const std::string& path) const;
template std::string ConfigManager::safeGet<std::string>(const YAML::Node& node, const std::string& key, const std::string& path, const std::string& defaultValue) const;
template bool ConfigManager::safeGet<bool>(const YAML::Node& node, const std::string& key, const std::string& path, const bool& defaultValue) const;

} // namespace flapi
