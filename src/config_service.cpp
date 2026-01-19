#include <filesystem>
#include <fstream>
#include <sstream>
#include <regex>
#include <algorithm>

#include <yaml-cpp/yaml.h>

#include "config_service.hpp"
#include "json_utils.hpp"
#include "path_utils.hpp"
#include "database_manager.hpp"
#include "cache_manager.hpp"
#include "sql_template_processor.hpp"
#include "arrow_metrics.hpp"

namespace flapi {

// ============================================================================
// Handler Class Implementations
// ============================================================================

// AuditLogHandler - Already has full implementation above namespace

// FilesystemHandler
FilesystemHandler::FilesystemHandler(std::shared_ptr<ConfigManager> config_manager)
    : config_manager_(config_manager) {}


crow::response FilesystemHandler::getFilesystemStructure(const crow::request& req) {
    try {
        crow::json::wvalue response;
        
        // Get base path and add to response
        auto base_path = config_manager_->getBasePath();
        response["base_path"] = base_path;
        
        // Check if config file exists  
        auto config_file_path = std::filesystem::path(base_path) / "flapi.yaml";
        if (!std::filesystem::exists(config_file_path)) {
            config_file_path = std::filesystem::path(base_path) / "flapi.yml";
        }
        response["config_file_exists"] = std::filesystem::exists(config_file_path);
        response["config_file"] = config_file_path.filename().string();
        
        // Get the template directory path and add to response
        auto template_path = config_manager_->getFullTemplatePath();
        response["template_path"] = template_path.string();
        
        // Build the file tree
        crow::json::wvalue::list tree;
        if (std::filesystem::exists(template_path) && std::filesystem::is_directory(template_path)) {
            buildDirectoryTree(template_path, template_path, tree);
        }
        
        response["tree"] = std::move(tree);
        
        return crow::response(200, response);
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

void FilesystemHandler::buildDirectoryTree(const std::filesystem::path& root_path,
                                           const std::filesystem::path& current_path,
                                           crow::json::wvalue::list& tree) {
    // Collect entries for sorting
    std::vector<std::filesystem::directory_entry> directories;
    std::vector<std::filesystem::directory_entry> files;
    
    for (const auto& entry : std::filesystem::directory_iterator(current_path)) {
        if (entry.is_directory()) {
            directories.push_back(entry);
        } else {
            files.push_back(entry);
        }
    }
    
    // Sort alphabetically
    auto comparator = [](const auto& a, const auto& b) {
        return a.path().filename().string() < b.path().filename().string();
    };
    std::sort(directories.begin(), directories.end(), comparator);
    std::sort(files.begin(), files.end(), comparator);
    
    // Add directories first
    for (const auto& dir_entry : directories) {
        crow::json::wvalue node;
        node["name"] = dir_entry.path().filename().string();
        node["type"] = "directory";
        node["path"] = std::filesystem::relative(dir_entry.path(), root_path).string();
        
        // Recursively build children
        crow::json::wvalue::list children;
        buildDirectoryTree(root_path, dir_entry.path(), children);
        node["children"] = std::move(children);
        
        tree.push_back(std::move(node));
    }
    
    // Then add files
    for (const auto& file_entry : files) {
        crow::json::wvalue node = buildFileNode(file_entry.path(), root_path);
        tree.push_back(std::move(node));
    }
}

crow::json::wvalue FilesystemHandler::buildFileNode(const std::filesystem::path& file_path,
                                                    const std::filesystem::path& root_path) {
    crow::json::wvalue node;
    node["name"] = file_path.filename().string();
    node["type"] = "file";
    node["path"] = std::filesystem::relative(file_path, root_path).string();
    node["extension"] = file_path.extension().string();
    
    // If it's a YAML file, parse it for additional metadata
    if (file_path.extension() == ".yaml" || file_path.extension() == ".yml") {
        try {
            // Use ExtendedYamlParser to support {{include}} directives and environment variables
            flapi::ExtendedYamlParser::IncludeConfig include_config;
            include_config.allow_environment_variables = true;
            include_config.allow_conditional_includes = true;
            
            flapi::ExtendedYamlParser parser(include_config);
            auto parse_result = parser.parseFile(file_path);
            
            if (!parse_result.success) {
                CROW_LOG_WARNING << "Failed to parse YAML file " << file_path << ": " << parse_result.error_message;
                return node;
            }
            
            YAML::Node yaml_content = parse_result.node;
            
            // Determine YAML type
            if (yaml_content["url-path"]) {
                // REST endpoint
                node["yaml_type"] = "endpoint";
                node["url_path"] = yaml_content["url-path"].as<std::string>();
                
                if (yaml_content["template-source"]) {
                    node["template_source"] = yaml_content["template-source"].as<std::string>();
                }
                
                // Check for cache configuration
                if (yaml_content["cache"] && yaml_content["cache"]["enabled"] && 
                    yaml_content["cache"]["enabled"].as<bool>()) {
                    if (yaml_content["cache"]["template-file"]) {
                        node["cache_template_source"] = yaml_content["cache"]["template-file"].as<std::string>();
                    }
                }
            } else if (yaml_content["mcp-tool"]) {
                // MCP tool
                node["yaml_type"] = "mcp-tool";
                if (yaml_content["mcp-tool"]["name"]) {
                    node["mcp_name"] = yaml_content["mcp-tool"]["name"].as<std::string>();
                }
                if (yaml_content["template-source"]) {
                    node["template_source"] = yaml_content["template-source"].as<std::string>();
                }
            } else if (yaml_content["mcp-resource"]) {
                // MCP resource
                node["yaml_type"] = "mcp-resource";
                if (yaml_content["mcp-resource"]["name"]) {
                    node["mcp_name"] = yaml_content["mcp-resource"]["name"].as<std::string>();
                }
                if (yaml_content["template-source"]) {
                    node["template_source"] = yaml_content["template-source"].as<std::string>();
                }
            } else if (yaml_content["mcp-prompt"]) {
                // MCP prompt
                node["yaml_type"] = "mcp-prompt";
                if (yaml_content["mcp-prompt"]["name"]) {
                    node["mcp_name"] = yaml_content["mcp-prompt"]["name"].as<std::string>();
                }
            } else {
                // Shared configuration file
                node["yaml_type"] = "shared";
            }
        } catch (const std::exception& e) {
            // If YAML parsing fails, just mark it as a regular file
            CROW_LOG_WARNING << "Failed to parse YAML file " << file_path << ": " << e.what();
        }
    }
    
    return node;
}

// SchemaHandler
SchemaHandler::SchemaHandler(std::shared_ptr<ConfigManager> config_manager)
    : config_manager_(config_manager) {}

// TemplateHandler
TemplateHandler::TemplateHandler(std::shared_ptr<ConfigManager> config_manager)
    : config_manager_(config_manager) {}

// CacheConfigHandler
CacheConfigHandler::CacheConfigHandler(std::shared_ptr<ConfigManager> config_manager)
    : config_manager_(config_manager) {}

// EndpointConfigHandler
EndpointConfigHandler::EndpointConfigHandler(std::shared_ptr<ConfigManager> config_manager)
    : config_manager_(config_manager) {}

// ProjectConfigHandler
ProjectConfigHandler::ProjectConfigHandler(std::shared_ptr<ConfigManager> config_manager)
    : config_manager_(config_manager) {}

// ============================================================================
// ConfigService Implementation (Facade)
// ============================================================================

ConfigService::ConfigService(std::shared_ptr<ConfigManager> config_manager,
                           bool enabled,
                           const std::string& auth_token)
    : config_manager(config_manager),
      audit_handler_(std::make_unique<AuditLogHandler>(config_manager)),
      filesystem_handler_(std::make_unique<FilesystemHandler>(config_manager)),
      schema_handler_(std::make_unique<SchemaHandler>(config_manager)),
      template_handler_(std::make_unique<TemplateHandler>(config_manager)),
      cache_handler_(std::make_unique<CacheConfigHandler>(config_manager)),
      endpoint_handler_(std::make_unique<EndpointConfigHandler>(config_manager)),
      project_handler_(std::make_unique<ProjectConfigHandler>(config_manager)),
      doc_generator_(nullptr),
      enabled_(enabled),
      auth_token_(auth_token) {}

void ConfigService::setDocGenerator(std::shared_ptr<OpenAPIDocGenerator> doc_gen) {
    doc_generator_ = doc_gen;
}

bool ConfigService::validateToken(const crow::request& req) const {
    // If service is not enabled, reject all requests
    if (!enabled_) {
        return false;
    }
    
    // Check Authorization header: "Bearer <token>"
    auto auth_header = req.get_header_value("Authorization");
    if (!auth_header.empty()) {
        const std::string bearer_prefix = "Bearer ";
        if (auth_header.substr(0, bearer_prefix.length()) == bearer_prefix) {
            std::string token = auth_header.substr(bearer_prefix.length());
            return token == auth_token_;
        }
    }
    
    // Also check X-Config-Token header for easier testing
    auto token_header = req.get_header_value("X-Config-Token");
    if (!token_header.empty()) {
        return token_header == auth_token_;
    }
    
    return false;
}

void ConfigService::registerRoutes(FlapiApp& app) {
    CROW_LOG_INFO << "Registering config routes";
    
    // If config service is not enabled, don't register routes
    if (!enabled_) {
        CROW_LOG_INFO << "Config service is disabled, skipping route registration";
        return;
    }

    // Project configuration routes
    CROW_ROUTE(app, "/api/v1/_config/project")
        .methods("GET"_method, "PUT"_method)
        ([this](const crow::request& req) {
            if (!validateToken(req)) {
                return crow::response(401, "Unauthorized: Invalid or missing token");
            }
            if (req.method == crow::HTTPMethod::Get)
                return project_handler_->getProjectConfig(req);
            else
                return project_handler_->updateProjectConfig(req);
        });

    // Environment variables route
    CROW_ROUTE(app, "/api/v1/_config/environment-variables")
        .methods("GET"_method)
        ([this](const crow::request& req) {
            if (!validateToken(req)) {
                return crow::response(401, "Unauthorized: Invalid or missing token");
            }
            return project_handler_->getEnvironmentVariables(req);
        });

    // Endpoints configuration routes
    CROW_ROUTE(app, "/api/v1/_config/endpoints")
        .methods("GET"_method, "POST"_method)
        ([this](const crow::request& req) {
            if (!validateToken(req)) {
                return crow::response(401, "Unauthorized: Invalid or missing token");
            }
            if (req.method == crow::HTTPMethod::Get)
                return endpoint_handler_->listEndpoints(req);
            else
                return endpoint_handler_->createEndpoint(req);
        });

    CROW_ROUTE(app, "/api/v1/_config/endpoints/<string>")
        .methods("GET"_method, "PUT"_method, "DELETE"_method)
        ([this](const crow::request& req, const std::string& slug) {
            if (!validateToken(req)) {
                return crow::response(401, "Unauthorized: Invalid or missing token");
            }
            // Use centralized slug lookup in endpoint handler
            switch (req.method) {
                case crow::HTTPMethod::Get:
                    return endpoint_handler_->getEndpointConfigBySlug(req, slug);
                case crow::HTTPMethod::Put:
                    return endpoint_handler_->updateEndpointConfigBySlug(req, slug);
                case crow::HTTPMethod::Delete:
                    return endpoint_handler_->deleteEndpointBySlug(req, slug);
                default:
                    return crow::response(405);
            }
        });

    // Endpoint validation and reload routes
    CROW_ROUTE(app, "/api/v1/_config/endpoints/<string>/validate")
        .methods("POST"_method)
        ([this](const crow::request& req, const std::string& slug) {
            if (!validateToken(req)) {
                return crow::response(401, "Unauthorized: Invalid or missing token");
            }
            const std::string path = PathUtils::slugToPath(slug);
            return endpoint_handler_->validateEndpointConfig(req, path);
        });

    CROW_ROUTE(app, "/api/v1/_config/endpoints/<string>/reload")
        .methods("POST"_method)
        ([this](const crow::request& req, const std::string& slug) {
            if (!validateToken(req)) {
                return crow::response(401, "Unauthorized: Invalid or missing token");
            }
            const std::string path = PathUtils::slugToPath(slug);
            return endpoint_handler_->reloadEndpointConfig(req, path);
        });

    // Parameters route
    CROW_ROUTE(app, "/api/v1/_config/endpoints/<string>/parameters")
        .methods("GET"_method)
        ([this](const crow::request& req, const std::string& slug) {
            if (!validateToken(req)) {
                return crow::response(401, "Unauthorized: Invalid or missing token");
            }
            const std::string path = PathUtils::slugToPath(slug);
            return endpoint_handler_->getEndpointParameters(req, path);
        });

    // Find endpoints by template file
    CROW_ROUTE(app, "/api/v1/_config/endpoints/by-template")
        .methods("POST"_method)
        ([this](const crow::request& req) {
            if (!validateToken(req)) {
                return crow::response(401, "Unauthorized: Invalid or missing token");
            }
            auto json = crow::json::load(req.body);
            if (!json || !json.has("template_path")) {
                return crow::response(400, "Invalid JSON: missing 'template_path' field");
            }
            const std::string template_path = json["template_path"].s();
            return endpoint_handler_->findEndpointsByTemplate(req, template_path);
        });

    // Template routes
    CROW_ROUTE(app, "/api/v1/_config/endpoints/<string>/template")
        .methods("GET"_method, "PUT"_method)
        ([this](const crow::request& req, const std::string& slug) {
            if (!validateToken(req)) {
                return crow::response(401, "Unauthorized: Invalid or missing token");
            }
            // Use slug-based lookup (works for both REST and MCP)
            if (req.method == crow::HTTPMethod::Get)
                return template_handler_->getEndpointTemplateBySlug(req, slug);
            else
                return template_handler_->updateEndpointTemplateBySlug(req, slug);
        });

    CROW_ROUTE(app, "/api/v1/_config/endpoints/<string>/template/expand")
        .methods("POST"_method)
        ([this](const crow::request& req, const std::string& slug) {
            if (!validateToken(req)) {
                return crow::response(401, "Unauthorized: Invalid or missing token");
            }
            // Use slug-based lookup (works for both REST and MCP)
            return template_handler_->expandTemplateBySlug(req, slug);
        });

    CROW_ROUTE(app, "/api/v1/_config/endpoints/<string>/template/test")
        .methods("POST"_method)
        ([this](const crow::request& req, const std::string& slug) {
            if (!validateToken(req)) {
                return crow::response(401, "Unauthorized: Invalid or missing token");
            }
            // Use slug-based lookup (works for both REST and MCP)
            return template_handler_->testTemplateBySlug(req, slug);
        });

    // Cache routes
    CROW_ROUTE(app, "/api/v1/_config/endpoints/<string>/cache")
        .methods("GET"_method, "PUT"_method)
        ([this](const crow::request& req, const std::string& slug) {
            if (!validateToken(req)) {
                return crow::response(401, "Unauthorized: Invalid or missing token");
            }
            const std::string path = PathUtils::slugToPath(slug);
            if (req.method == crow::HTTPMethod::Get)
                return cache_handler_->getCacheConfig(req, path);
            else
                return cache_handler_->updateCacheConfig(req, path);
        });

    CROW_ROUTE(app, "/api/v1/_config/endpoints/<string>/cache/template")
        .methods("GET"_method, "PUT"_method)
        ([this](const crow::request& req, const std::string& slug) {
            if (!validateToken(req)) {
                return crow::response(401, "Unauthorized: Invalid or missing token");
            }
            const std::string path = PathUtils::slugToPath(slug);
            if (req.method == crow::HTTPMethod::Get)
                return template_handler_->getCacheTemplate(req, path);
            else
                return template_handler_->updateCacheTemplate(req, path);
        });

    CROW_ROUTE(app, "/api/v1/_config/endpoints/<string>/cache/refresh")
        .methods("POST"_method)
        ([this](const crow::request& req, const std::string& slug) {
            if (!validateToken(req)) {
                return crow::response(401, "Unauthorized: Invalid or missing token");
            }
            const std::string path = PathUtils::slugToPath(slug);
            return cache_handler_->refreshCache(req, path);
        });

    CROW_ROUTE(app, "/api/v1/_config/endpoints/<string>/cache/gc")
        .methods("POST"_method)
        ([this](const crow::request& req, const std::string& slug) {
            if (!validateToken(req)) {
                return crow::response(401, "Unauthorized: Invalid or missing token");
            }
            const std::string path = PathUtils::slugToPath(slug);
            return cache_handler_->performGarbageCollection(req, path);
        });

    // DuckLake audit routes
    CROW_ROUTE(app, "/api/v1/_config/endpoints/<string>/cache/audit")
        .methods("GET"_method)
        ([this](const crow::request& req, const std::string& slug) {
            if (!validateToken(req)) {
                return crow::response(401, "Unauthorized: Invalid or missing token");
            }
            const std::string path = PathUtils::slugToPath(slug);
            return audit_handler_->getCacheAuditLog(path);
        });

    CROW_ROUTE(app, "/api/v1/_config/cache/audit")
        .methods("GET"_method)
        ([this](const crow::request& req) {
            if (!validateToken(req)) {
                return crow::response(401, "Unauthorized: Invalid or missing token");
            }
            return audit_handler_->getAllCacheAuditLogs();
        });

    // Schema routes
    CROW_ROUTE(app, "/api/v1/_config/schema")
        .methods("GET"_method)
        ([this](const crow::request& req) {
            if (!validateToken(req)) {
                return crow::response(401, "Unauthorized: Invalid or missing token");
            }
            return schema_handler_->getSchema(req);
        });

    CROW_ROUTE(app, "/api/v1/_config/schema/refresh")
        .methods("POST"_method)
        ([this](const crow::request& req) {
            if (!validateToken(req)) {
                return crow::response(401, "Unauthorized: Invalid or missing token");
            }
            return schema_handler_->refreshSchema(req);
        });

    // Filesystem routes
    CROW_ROUTE(app, "/api/v1/_config/filesystem")
        .methods("GET"_method)
        ([this](const crow::request& req) {
            if (!validateToken(req)) {
                return crow::response(401, "Unauthorized: Invalid or missing token");
            }
            return filesystem_handler_->getFilesystemStructure(req);
        });
    
    // Log level control routes
    CROW_ROUTE(app, "/api/v1/_config/log-level")
        .methods("GET"_method, "PUT"_method)
        ([this](const crow::request& req) {
            if (!validateToken(req)) {
                return crow::response(401, "Unauthorized: Invalid or missing token");
            }
            
            if (req.method == crow::HTTPMethod::Get) {
                // Get current log level
                crow::json::wvalue response;
                response["level"] = current_log_level_;
                return crow::response(200, response);
            } else {
                // Set new log level
                auto json = crow::json::load(req.body);
                if (!json || !json.has("level")) {
                    return crow::response(400, "Missing 'level' field");
                }
                
                std::string level_str = json["level"].s();
                
                // Validate and set log level
                if (level_str == "debug") {
                    crow::logger::setLogLevel(crow::LogLevel::Debug);
                    current_log_level_ = "debug";
                } else if (level_str == "info") {
                    crow::logger::setLogLevel(crow::LogLevel::Info);
                    current_log_level_ = "info";
                } else if (level_str == "warning") {
                    crow::logger::setLogLevel(crow::LogLevel::Warning);
                    current_log_level_ = "warning";
                } else if (level_str == "error") {
                    crow::logger::setLogLevel(crow::LogLevel::Error);
                    current_log_level_ = "error";
                } else {
                    return crow::response(400, "Invalid log level. Use: debug, info, warning, error");
                }
                
                CROW_LOG_INFO << "Log level changed to: " << current_log_level_;
                
                crow::json::wvalue response;
                response["level"] = current_log_level_;
                response["message"] = "Log level updated successfully";
                return crow::response(200, response);
            }
        });
    
    // Health check endpoint (no authentication required)
    CROW_ROUTE(app, "/api/v1/_config/health")
        .methods("GET"_method)
        ([this](const crow::request& /* req */) {
            crow::json::wvalue health;

            // Server status
            health["status"] = "healthy";
            health["server"]["name"] = "flapi";
            health["server"]["version"] = "1.0.0";

            // Database status
            auto db_manager = DatabaseManager::getInstance();
            health["database"]["status"] = db_manager ? "connected" : "disconnected";

            // Endpoints count
            if (config_manager) {
                health["endpoints"]["count"] = static_cast<int>(config_manager->getEndpoints().size());
            }

            // Arrow IPC status
            auto& arrowMetrics = ArrowMetrics::instance();
            crow::json::wvalue arrow;
            arrow["enabled"] = true;
            arrow["codecs"] = crow::json::wvalue::list({"lz4", "zstd"});

            // Arrow counters
            crow::json::wvalue counters;
            counters["total_requests"] = arrowMetrics.counters.totalRequests.load();
            counters["successful_requests"] = arrowMetrics.counters.successfulRequests.load();
            counters["failed_requests"] = arrowMetrics.counters.failedRequests.load();
            counters["total_rows"] = arrowMetrics.counters.totalRows.load();
            counters["total_batches"] = arrowMetrics.counters.totalBatches.load();
            counters["total_bytes_written"] = arrowMetrics.counters.totalBytesWritten.load();
            counters["total_bytes_compressed"] = arrowMetrics.counters.totalBytesCompressed.load();
            counters["compression_requests"] = arrowMetrics.counters.compressionRequests.load();
            arrow["counters"] = std::move(counters);

            // Arrow gauges
            crow::json::wvalue gauges;
            gauges["active_streams"] = arrowMetrics.gauges.activeStreams.load();
            gauges["peak_active_streams"] = arrowMetrics.gauges.peakActiveStreams.load();
            gauges["current_memory_bytes"] = arrowMetrics.gauges.currentMemoryUsage.load();
            gauges["peak_memory_bytes"] = arrowMetrics.gauges.peakMemoryUsage.load();
            arrow["gauges"] = std::move(gauges);

            // Arrow histograms (calculated values)
            crow::json::wvalue stats;
            stats["avg_duration_us"] = arrowMetrics.getAverageDurationUs();
            stats["avg_compression_ratio"] = arrowMetrics.getAverageCompressionRatio();
            uint64_t minDuration = arrowMetrics.histograms.minDurationUs.load();
            uint64_t maxDuration = arrowMetrics.histograms.maxDurationUs.load();
            if (minDuration != UINT64_MAX) {
                stats["min_duration_us"] = minDuration;
            }
            if (maxDuration > 0) {
                stats["max_duration_us"] = maxDuration;
            }
            uint64_t minBatch = arrowMetrics.histograms.minBatchRows.load();
            uint64_t maxBatch = arrowMetrics.histograms.maxBatchRows.load();
            if (minBatch != UINT64_MAX) {
                stats["min_batch_rows"] = minBatch;
            }
            if (maxBatch > 0) {
                stats["max_batch_rows"] = maxBatch;
            }
            arrow["stats"] = std::move(stats);

            health["arrow"] = std::move(arrow);

            return crow::response(200, health);
        });

    // OpenAPI documentation route
    CROW_ROUTE(app, "/api/v1/doc.yaml")
        .methods("GET"_method)
        ([this, &app](const crow::request& req) {
            if (!doc_generator_) {
                return crow::response(503, "Documentation generator not initialized");
            }
            
            try {
                YAML::Node doc = doc_generator_->generateConfigServiceDoc(const_cast<FlapiApp&>(app));
                std::stringstream ss;
                ss << doc;
                
                crow::response resp(200, ss.str());
                resp.set_header("Content-Type", "application/x-yaml");
                resp.set_header("Content-Disposition", "inline; filename=\"flapi-config-api.yaml\"");
                return resp;
            } catch (const std::exception& e) {
                CROW_LOG_ERROR << "Error generating OpenAPI documentation: " << e.what();
                return crow::response(500, std::string("Error generating documentation: ") + e.what());
            }
        });
}

// ============================================================================
// AuditLogHandler Implementation
// ============================================================================

AuditLogHandler::AuditLogHandler(std::shared_ptr<ConfigManager> config_manager)
    : config_manager_(config_manager) {}

crow::response AuditLogHandler::getCacheAuditLog(const std::string& path) {
    try {
        const auto* endpoint = config_manager_->getEndpointForPath(path);
        if (!endpoint) {
            return crow::response(404, "Endpoint not found");
        }

        if (!endpoint->cache.enabled) {
            return crow::response(400, "Cache not enabled for this endpoint");
        }

        const auto& ducklake_config = config_manager_->getDuckLakeConfig();
        if (!ducklake_config.enabled) {
            return crow::response(400, "DuckLake not enabled");
        }

        auto db_manager = DatabaseManager::getInstance();
        const std::string catalog = ducklake_config.alias;
        
        std::string audit_query = buildAuditQuery(catalog, path);
        std::map<std::string, std::string> params;
        auto result = db_manager->executeDuckLakeQuery(audit_query, params);
        
        return crow::response(200, result.data);
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

crow::response AuditLogHandler::getAllCacheAuditLogs() {
    try {
        const auto& ducklake_config = config_manager_->getDuckLakeConfig();
        if (!ducklake_config.enabled) {
            return crow::response(400, "DuckLake not enabled");
        }

        auto db_manager = DatabaseManager::getInstance();
        const std::string catalog = ducklake_config.alias;
        
        std::string audit_query = buildAuditQuery(catalog);
        std::map<std::string, std::string> params;
        auto result = db_manager->executeDuckLakeQuery(audit_query, params);
        
        return crow::response(200, result.data);
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

std::string AuditLogHandler::buildAuditQuery(const std::string& catalog, 
                                               const std::string& endpoint_filter) const {
    std::ostringstream query;
    query << R"(
        SELECT event_id, endpoint_path, cache_table, cache_schema, sync_type, status, message,
               snapshot_id, rows_affected, sync_started_at, sync_completed_at, duration_ms
        FROM )" << catalog << R"(.audit.sync_events)";
    
    if (!endpoint_filter.empty()) {
        query << "\n        WHERE endpoint_path = '" << endpoint_filter << "'";
    }
    
    query << R"(
        ORDER BY sync_started_at DESC
        LIMIT )" << (endpoint_filter.empty() ? "500" : "100");
    
    return query.str();
}

crow::response ProjectConfigHandler::getProjectConfig(const crow::request& req) {
    try {
        if (!config_manager_) {
            return crow::response(500, "Internal server error: Configuration manager is not initialized");
        }
        auto config = config_manager_->getFlapiConfig();
        return crow::response(200, config);
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

crow::response ProjectConfigHandler::updateProjectConfig(const crow::request& req) {
    try {
        auto json = crow::json::load(req.body);
        if (!json)
            return crow::response(400, "Invalid JSON");

        // TODO: Implement project config update logic
        return crow::response(501, "Not implemented");
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

crow::response ProjectConfigHandler::getEnvironmentVariables(const crow::request& req) {
    try {
        crow::json::wvalue response;
        response["variables"] = crow::json::wvalue::list();
        
        // Get template environment whitelist
        const auto& template_config = config_manager_->getTemplateConfig();
        size_t idx = 0;
        
        for (const auto& var_name : template_config.environment_whitelist) {
            crow::json::wvalue var;
            var["name"] = var_name;
            
            // Try to get actual value from environment
            const char* env_value = std::getenv(var_name.c_str());
            if (env_value) {
                var["value"] = std::string(env_value);
                var["available"] = true;
            } else {
                var["value"] = "";
                var["available"] = false;
            }
            
            response["variables"][idx++] = std::move(var);
        }
        
        return crow::response(200, response);
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

crow::response EndpointConfigHandler::listEndpoints(const crow::request& req) {
    try {
        auto endpoints = config_manager_->getEndpointsConfig();
        return crow::response(200, endpoints);
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

crow::response EndpointConfigHandler::createEndpoint(const crow::request& req) {
    try {
        auto json = crow::json::load(req.body);
        if (!json)
            return crow::response(400, "Invalid JSON");

        auto endpoint = jsonToEndpointConfig(json);
        config_manager_->addEndpoint(endpoint);
        
        return crow::response(201);
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

// Helper method to find endpoint by slug (centralized slug logic)
const EndpointConfig* findEndpointBySlug(std::shared_ptr<ConfigManager> config_manager, const std::string& slug) {
    const auto& endpoints = config_manager->getEndpoints();
    for (const auto& endpoint : endpoints) {
        if (endpoint.getSlug() == slug) {
            return &endpoint;
        }
    }
    return nullptr;
}

// New slug-based methods (centralized, works for both REST and MCP)
crow::response EndpointConfigHandler::getEndpointConfigBySlug(const crow::request& req, const std::string& slug) {
    try {
        const auto* endpoint = findEndpointBySlug(config_manager_, slug);
        if (!endpoint) {
            return crow::response(404, "Endpoint not found");
        }
        return crow::response(200, endpointConfigToJson(*endpoint));
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

crow::response EndpointConfigHandler::updateEndpointConfigBySlug(const crow::request& req, const std::string& slug) {
    try {
        auto json = crow::json::load(req.body);
        if (!json) {
            return crow::response(400, "Invalid JSON");
        }

        auto updated_config = jsonToEndpointConfig(json);
        
        // Verify slug matches
        if (updated_config.getSlug() != slug) {
            return crow::response(400, "Slug in config does not match endpoint slug");
        }

        if (!config_manager_->replaceEndpoint(updated_config)) {
            return crow::response(404, "Endpoint not found");
        }

        return crow::response(200);
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

crow::response EndpointConfigHandler::deleteEndpointBySlug(const crow::request& req, const std::string& slug) {
    try {
        const auto* endpoint = findEndpointBySlug(config_manager_, slug);
        if (!endpoint) {
            return crow::response(404, "Endpoint not found");
        }
        
        // Use the endpoint's name for removal
        if (!config_manager_->removeEndpointByPath(endpoint->getName())) {
            return crow::response(404, "Endpoint not found");
        }

        return crow::response(200);
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

// Legacy path-based method (kept for backward compatibility)
crow::response EndpointConfigHandler::getEndpointConfig(const crow::request& req, const std::string& path) {
    try {
        const auto* endpoint = config_manager_->getEndpointForPath(path);
        if (!endpoint) {
            return crow::response(404, "Endpoint not found");
        }

        return crow::response(200, endpointConfigToJson(*endpoint));
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

// Helper methods for converting between JSON and EndpointConfig
crow::json::wvalue EndpointConfigHandler::endpointConfigToJson(const EndpointConfig& config) {
    return config_manager_->serializeEndpointConfig(config, EndpointJsonStyle::HyphenCase);
}

EndpointConfig EndpointConfigHandler::jsonToEndpointConfig(const crow::json::rvalue& json) {
    return config_manager_->deserializeEndpointConfig(json);
}

// Implement remaining endpoint handlers...
crow::response EndpointConfigHandler::updateEndpointConfig(const crow::request& req, const std::string& path) {
    try {
        auto json = crow::json::load(req.body);
        if (!json) {
            return crow::response(400, "Invalid JSON");
        }

        auto updated_config = jsonToEndpointConfig(json);

        if (updated_config.urlPath != path) {
            return crow::response(400, "URL path in config does not match endpoint path");
        }

        if (!config_manager_->replaceEndpoint(updated_config)) {
            return crow::response(404, "Endpoint not found");
        }

        return crow::response(200);
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

crow::response EndpointConfigHandler::deleteEndpoint(const crow::request& req, const std::string& path) {
    try {
        if (!config_manager_->removeEndpointByPath(path)) {
            return crow::response(404, "Endpoint not found");
        }

        return crow::response(200);
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

crow::response EndpointConfigHandler::validateEndpointConfig(const crow::request& req, const std::string& path) {
    try {
        std::string yaml_content = req.body;
        if (yaml_content.empty()) {
            return crow::response(400, "Empty request body");
        }

        auto validation = config_manager_->validateEndpointConfigFromYaml(yaml_content);
        
        crow::json::wvalue response;
        response["valid"] = validation.valid;
        
        if (!validation.errors.empty()) {
            response["errors"] = crow::json::wvalue::list();
            for (size_t i = 0; i < validation.errors.size(); ++i) {
                response["errors"][i] = validation.errors[i];
            }
        }
        
        if (!validation.warnings.empty()) {
            response["warnings"] = crow::json::wvalue::list();
            for (size_t i = 0; i < validation.warnings.size(); ++i) {
                response["warnings"][i] = validation.warnings[i];
            }
        }
        
        int32_t status_code = validation.valid ? 200 : 400;
        return crow::response(status_code, response);
    } catch (const std::exception& e) {
        crow::json::wvalue response;
        response["valid"] = false;
        response["errors"] = crow::json::wvalue::list();
        response["errors"][0] = std::string("Validation error: ") + e.what();
        return crow::response(400, response);
    }
}

crow::response EndpointConfigHandler::reloadEndpointConfig(const crow::request& req, const std::string& path) {
    try {
        bool success = config_manager_->reloadEndpointConfig(path);
        
        crow::json::wvalue response;
        response["success"] = success;
        response["message"] = success ? 
            "Endpoint configuration reloaded successfully" : 
            "Failed to reload endpoint configuration";
        
        return crow::response(success ? 200 : 404, response);
    } catch (const std::exception& e) {
        crow::json::wvalue response;
        response["success"] = false;
        response["message"] = std::string("Reload error: ") + e.what();
        return crow::response(500, response);
    }
}

crow::response EndpointConfigHandler::getEndpointParameters(const crow::request& req, const std::string& path) {
    try {
        const auto* endpoint = config_manager_->getEndpointForPath(path);
        if (!endpoint) {
            return crow::response(404, "Endpoint not found");
        }

        crow::json::wvalue response;
        response["parameters"] = crow::json::wvalue::list();
        
        // Extract request field definitions
        size_t idx = 0;
        for (const auto& field : endpoint->request_fields) {
            crow::json::wvalue param;
            param["name"] = field.fieldName;
            param["in"] = field.fieldIn;
            param["description"] = field.description;
            param["required"] = field.required;
            
            if (!field.defaultValue.empty()) {
                param["default"] = field.defaultValue;
            }
            
            // Add validator info if present
            if (!field.validators.empty()) {
                crow::json::wvalue validators_list = crow::json::wvalue::list();
                for (const auto& validator : field.validators) {
                    crow::json::wvalue v;
                    v["type"] = validator.type;
                    
                    if (validator.type == "int") {
                        v["min"] = validator.min;
                        v["max"] = validator.max;
                    } else if (validator.type == "string" && !validator.regex.empty()) {
                        v["regex"] = validator.regex;
                    } else if (validator.type == "enum") {
                        crow::json::wvalue allowed = crow::json::wvalue::list();
                        for (const auto& val : validator.allowedValues) {
                            allowed[allowed.size()] = val;
                        }
                        v["allowedValues"] = std::move(allowed);
                    }
                    
                    validators_list[validators_list.size()] = std::move(v);
                }
                param["validators"] = std::move(validators_list);
            }
            
            response["parameters"][idx++] = std::move(param);
        }
        
        // Add endpoint metadata
        response["endpoint"] = endpoint->getName();
        response["method"] = endpoint->method;
        
        return crow::response(200, response);
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

crow::response EndpointConfigHandler::findEndpointsByTemplate(const crow::request& req, const std::string& template_path) {
    try {
        crow::json::wvalue response;
        response["endpoints"] = crow::json::wvalue::list();
        
        // Normalize the template path for comparison
        auto normalized_template = std::filesystem::path(template_path).lexically_normal();
        
        // Search through all endpoints
        const auto& endpoints = config_manager_->getEndpoints();
        size_t idx = 0;
        
        for (const auto& endpoint : endpoints) {
            // Normalize endpoint's template path
            auto endpoint_template = std::filesystem::path(endpoint.templateSource).lexically_normal();
            
            // Check if paths match
            if (endpoint_template == normalized_template) {
                crow::json::wvalue ep;
                ep["url_path"] = endpoint.urlPath;
                ep["method"] = endpoint.method;
                ep["config_file_path"] = endpoint.config_file_path;
                ep["template_source"] = endpoint.templateSource;
                
                // Add endpoint type info
                if (endpoint.isRESTEndpoint()) {
                    ep["type"] = "REST";
                } else if (endpoint.isMCPTool()) {
                    ep["type"] = "MCP_Tool";
                    ep["mcp_name"] = endpoint.mcp_tool->name;
                } else if (endpoint.isMCPResource()) {
                    ep["type"] = "MCP_Resource";
                    ep["mcp_name"] = endpoint.mcp_resource->name;
                } else if (endpoint.isMCPPrompt()) {
                    ep["type"] = "MCP_Prompt";
                    ep["mcp_name"] = endpoint.mcp_prompt->name;
                }
                
                // TODO: Include full configuration to avoid additional API calls
                // Currently disabled due to Crow JSON serialization issues in tests
                // ep["full_config"] = config_manager_->serializeEndpointConfig(endpoint, EndpointJsonStyle::HyphenCase);
                
                response["endpoints"][idx++] = std::move(ep);
            }
        }
        
        response["count"] = static_cast<int>(idx);
        response["template_path"] = template_path;
        
        return crow::response(200, response);
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

crow::response TemplateHandler::getEndpointTemplate(const crow::request& req, const std::string& path) {
    try {
        // Find the endpoint
        auto* endpoint = config_manager_->getEndpointForPath(path);
        if (!endpoint) {
            return crow::response(404, "Endpoint not found");
        }

        // Read the template file
        auto template_path = resolveTemplatePath(endpoint->templateSource);
        std::ifstream file(template_path);
        if (!file.is_open()) {
            return crow::response(500, "Could not open template file: " + template_path.string());
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        
        crow::json::wvalue response;
        response["template"] = buffer.str();
        return crow::response(200, response);
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

crow::response TemplateHandler::updateEndpointTemplate(const crow::request& req, const std::string& path) {
    try {
        auto json = crow::json::load(req.body);
        if (!json || !json.has("template")) {
            return crow::response(400, "Invalid JSON: missing 'template' field");
        }

        // Find the endpoint
        auto* endpoint = config_manager_->getEndpointForPath(path);
        if (!endpoint) {
            return crow::response(404, "Endpoint not found");
        }

        // Write the template to file
        auto template_path = resolveTemplatePath(endpoint->templateSource);
        std::ofstream file(template_path);
        if (!file.is_open()) {
            return crow::response(500, "Could not open template file for writing: " + template_path.string());
        }

        file << json["template"].s();
        if (!file.good()) {
            return crow::response(500, "Error writing template to file");
        }

        return crow::response(200);
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

crow::response TemplateHandler::expandTemplate(const crow::request& req, const std::string& path) {
    try {
        auto json = crow::json::load(req.body);
        if (!json || !json.has("parameters")) {
            return crow::response(400, "Invalid JSON: missing 'parameters' field");
        }

        // Check query parameters
        auto url_params = req.url_params;
        bool include_variables = url_params.get("include_variables") != nullptr;
        bool validate_only = url_params.get("validate_only") != nullptr;

        // Find the endpoint
        auto* endpoint = config_manager_->getEndpointForPath(path);
        if (!endpoint) {
            return crow::response(404, "Endpoint not found");
        }

        // Convert parameters to map
        std::map<std::string, std::string> params;
        for (const auto& param : json["parameters"]) {
            // Convert all parameter values to strings using JsonUtils
            std::string value = JsonUtils::valueToString(param);
            params[param.key()] = value;
        }

        // Use SQLTemplateProcessor to expand the template
        auto sql_processor = std::make_shared<SQLTemplateProcessor>(config_manager_);
        
        // If validation only is requested, perform validation checks
        if (validate_only) {
            crow::json::wvalue validation_response;
            crow::json::wvalue errors = crow::json::wvalue::list();
            crow::json::wvalue warnings = crow::json::wvalue::list();
            bool is_valid = true;
            
            try {
                // 1. Validate Mustache template syntax by attempting to load it
                std::string template_content;
                try {
                    template_content = sql_processor->loadTemplate(*endpoint);
                } catch (const std::exception& e) {
                    crow::json::wvalue error;
                    error["type"] = "template_load";
                    error["message"] = std::string("Failed to load template: ") + e.what();
                    errors[errors.size()] = std::move(error);
                    is_valid = false;
                }
                
                // 2. Validate variable references by creating template context
                if (is_valid) {
                    try {
                        std::map<std::string, std::string> context_params = params;
                        crow::mustache::context ctx = sql_processor->createTemplateContext(*endpoint, context_params);
                        
                        // Check if all variables in template are available in context
                        // This is a simple check - we could enhance this with proper Mustache parsing
                        std::regex var_regex(R"(\{\{([^}]+)\}\})");
                        std::sregex_iterator iter(template_content.begin(), template_content.end(), var_regex);
                        std::sregex_iterator end;
                        
                        for (; iter != end; ++iter) {
                            std::string var_name = (*iter)[1].str();
                            // Remove whitespace
                            var_name.erase(std::remove_if(var_name.begin(), var_name.end(), ::isspace), var_name.end());
                            
                            // Check if variable exists in context (simplified check)
                            std::string context_dump = ctx.dump();
                            if (context_dump.find("\"" + var_name + "\"") == std::string::npos) {
                                crow::json::wvalue warning;
                                warning["type"] = "undefined_variable";
                                warning["message"] = "Variable '" + var_name + "' may not be defined in context";
                                warning["variable"] = var_name;
                                warnings[warnings.size()] = std::move(warning);
                            }
                        }
                    } catch (const std::exception& e) {
                        crow::json::wvalue error;
                        error["type"] = "context_creation";
                        error["message"] = std::string("Failed to create template context: ") + e.what();
                        errors[errors.size()] = std::move(error);
                        is_valid = false;
                    }
                }
                
                // 3. Validate SQL by expanding and checking with DuckDB
                if (is_valid) {
                    try {
                        std::string expanded_sql = sql_processor->loadAndProcessTemplate(*endpoint, params);
                        
                        // Use DuckDB to validate the SQL syntax
                        auto db_manager = DatabaseManager::getInstance();
                        try {
                            // Use EXPLAIN to validate SQL without executing it
                            std::string explain_query = "EXPLAIN " + expanded_sql;
                            auto result = db_manager->executeQuery(explain_query, {}, false);
                            // If we get here, the SQL is valid
                        } catch (const std::exception& sql_error) {
                            crow::json::wvalue error;
                            error["type"] = "sql_syntax";
                            error["message"] = std::string("SQL validation failed: ") + sql_error.what();
                            errors[errors.size()] = std::move(error);
                            is_valid = false;
                        }
                    } catch (const std::exception& e) {
                        crow::json::wvalue error;
                        error["type"] = "template_expansion";
                        error["message"] = std::string("Failed to expand template: ") + e.what();
                        errors[errors.size()] = std::move(error);
                        is_valid = false;
                    }
                }
                
            } catch (const std::exception& e) {
                crow::json::wvalue error;
                error["type"] = "validation";
                error["message"] = std::string("Validation error: ") + e.what();
                errors[errors.size()] = std::move(error);
                is_valid = false;
            }
            
            validation_response["valid"] = is_valid;
            validation_response["errors"] = std::move(errors);
            validation_response["warnings"] = std::move(warnings);
            
            return crow::response(200, validation_response);
        }
        
        // Normal template expansion
        std::string expanded = sql_processor->loadAndProcessTemplate(*endpoint, params);

        crow::json::wvalue response;
        response["expanded"] = expanded;

        // Include variable metadata if requested
        if (include_variables) {
            // Create a copy of params for context creation (since it might be modified)
            std::map<std::string, std::string> context_params = params;
            crow::mustache::context ctx = sql_processor->createTemplateContext(*endpoint, context_params);
            
            // Convert context to JSON for variables metadata
            crow::json::wvalue variables;
            
            // Extract variables from the context
            std::string context_dump = ctx.dump();
            auto context_json = crow::json::load(context_dump);
            
            if (context_json) {
                // Add request parameters (from params namespace)
                if (context_json.has("params")) {
                    crow::json::wvalue request_vars;
                    for (const auto& key : context_json["params"].keys()) {
                        crow::json::wvalue var_info;
                        var_info["type"] = "string";
                        var_info["value"] = context_json["params"][key].s();
                        var_info["source"] = "request";
                        request_vars[key] = std::move(var_info);
                    }
                    variables["params"] = std::move(request_vars);
                }
                
                // Add connection variables
                if (context_json.has("conn")) {
                    crow::json::wvalue conn_vars;
                    for (const auto& key : context_json["conn"].keys()) {
                        crow::json::wvalue var_info;
                        var_info["type"] = "string";
                        var_info["value"] = context_json["conn"][key].s();
                        var_info["source"] = "connection";
                        conn_vars[key] = std::move(var_info);
                    }
                    variables["conn"] = std::move(conn_vars);
                }
                
                // Add environment variables
                if (context_json.has("env")) {
                    crow::json::wvalue env_vars;
                    for (const auto& key : context_json["env"].keys()) {
                        crow::json::wvalue var_info;
                        var_info["type"] = "string";
                        var_info["value"] = context_json["env"][key].s();
                        var_info["source"] = "environment";
                        env_vars[key] = std::move(var_info);
                    }
                    variables["env"] = std::move(env_vars);
                }
                
                // Add cache variables
                if (context_json.has("cache")) {
                    crow::json::wvalue cache_vars;
                    for (const auto& key : context_json["cache"].keys()) {
                        crow::json::wvalue var_info;
                        var_info["type"] = "string";
                        var_info["value"] = context_json["cache"][key].s();
                        var_info["source"] = "cache";
                        cache_vars[key] = std::move(var_info);
                    }
                    variables["cache"] = std::move(cache_vars);
                }
            }
            
            response["variables"] = std::move(variables);
        }

        return crow::response(200, response);
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

crow::response TemplateHandler::testTemplate(const crow::request& req, const std::string& path) {
    try {
        auto json = crow::json::load(req.body);
        if (!json || !json.has("parameters")) {
            return crow::response(400, "Invalid JSON: missing 'parameters' field");
        }

        // Find the endpoint
        auto* endpoint = config_manager_->getEndpointForPath(path);
        if (!endpoint) {
            return crow::response(404, "Endpoint not found");
        }

        if (endpoint->connection.empty()) {
            return crow::response(400, "Endpoint has no database connection configured");
        }

        // Convert parameters to map
        std::map<std::string, std::string> params;
        for (const auto& param : json["parameters"]) {
            // Convert all parameter values to strings using JsonUtils
            std::string value = JsonUtils::valueToString(param);
            params[param.key()] = value;
        }

        // Get database manager instance
        auto db_manager = DatabaseManager::getInstance();

        // Execute the query with a limit for safety
        params["limit"] = "10";  // Force a limit for test queries
        params["offset"] = "0";
        
        try {
            auto result = db_manager->executeQuery(*endpoint, params);
            
            // Create base response
            crow::json::wvalue response;
            response["success"] = true;
            
            // Handle empty result case
            if (result.data.t() == crow::json::type::Null || result.data.size() == 0) {
                response["columns"] = std::vector<std::string>();
                response["rows"] = std::vector<crow::json::wvalue>();
                return crow::response(200, response);
            }

            response["rows"] = std::move(result.data);
            response["columns"] = response["rows"][0].keys();
            
            return crow::response(200, response);

        } catch (const std::exception& e) {
            return crow::response(400, std::string("SQL execution error: ") + e.what());
        }
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

// Slug-based wrapper methods for TemplateHandler
crow::response TemplateHandler::getEndpointTemplateBySlug(const crow::request& req, const std::string& slug) {
    const auto* endpoint = findEndpointBySlug(config_manager_, slug);
    if (!endpoint) {
        return crow::response(404, "Endpoint not found");
    }
    return getEndpointTemplate(req, endpoint->getName());
}

crow::response TemplateHandler::updateEndpointTemplateBySlug(const crow::request& req, const std::string& slug) {
    const auto* endpoint = findEndpointBySlug(config_manager_, slug);
    if (!endpoint) {
        return crow::response(404, "Endpoint not found");
    }
    return updateEndpointTemplate(req, endpoint->getName());
}

crow::response TemplateHandler::expandTemplateBySlug(const crow::request& req, const std::string& slug) {
    const auto* endpoint = findEndpointBySlug(config_manager_, slug);
    if (!endpoint) {
        return crow::response(404, "Endpoint not found");
    }
    return expandTemplate(req, endpoint->getName());
}

crow::response TemplateHandler::testTemplateBySlug(const crow::request& req, const std::string& slug) {
    const auto* endpoint = findEndpointBySlug(config_manager_, slug);
    if (!endpoint) {
        return crow::response(404, "Endpoint not found");
    }
    return testTemplate(req, endpoint->getName());
}

crow::response CacheConfigHandler::getCacheConfig(const crow::request& req, const std::string& path) {
    try {
        auto* endpoint = config_manager_->getEndpointForPath(path);
        if (!endpoint) {
            return crow::response(404, "Endpoint not found");
        }

        crow::json::wvalue response;
        response["enabled"] = endpoint->cache.enabled;
        response["table"] = endpoint->cache.table;
        response["schema"] = endpoint->cache.schema;
        if (endpoint->cache.schedule) {
            response["schedule"] = endpoint->cache.schedule.value();
        }
        if (!endpoint->cache.primary_keys.empty()) {
            auto pk_list = crow::json::wvalue::list();
            for (const auto& pk : endpoint->cache.primary_keys) {
                pk_list.push_back(pk);
            }
            response["primary-key"] = std::move(pk_list);
        }
        if (endpoint->cache.cursor) {
            crow::json::wvalue cursor;
            cursor["column"] = endpoint->cache.cursor->column;
            cursor["type"] = endpoint->cache.cursor->type;
            response["cursor"] = std::move(cursor);
        }
        if (endpoint->cache.rollback_window) {
            response["rollback-window"] = endpoint->cache.rollback_window.value();
        }
        if (endpoint->cache.retention.keep_last_snapshots || endpoint->cache.retention.max_snapshot_age) {
            crow::json::wvalue retention;
            if (endpoint->cache.retention.keep_last_snapshots) {
                retention["keep-last-snapshots"] = static_cast<int64_t>(endpoint->cache.retention.keep_last_snapshots.value());
            }
            if (endpoint->cache.retention.max_snapshot_age) {
                retention["max-snapshot-age"] = endpoint->cache.retention.max_snapshot_age.value();
            }
            response["retention"] = std::move(retention);
        }
        if (endpoint->cache.delete_handling) {
            response["delete-handling"] = endpoint->cache.delete_handling.value();
        }
        if (endpoint->cache.template_file) {
            response["template-file"] = endpoint->cache.template_file.value();
        }

        return crow::response(200, response);
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

crow::response CacheConfigHandler::updateCacheConfig(const crow::request& req, const std::string& path) {
    try {
        auto json = crow::json::load(req.body);
        if (!json) {
            return crow::response(400, "Invalid JSON");
        }

        auto* endpoint = config_manager_->getEndpointForPath(path);
        if (!endpoint) {
            return crow::response(404, "Endpoint not found");
        }

        auto& cache = const_cast<CacheConfig&>(endpoint->cache);
        bool enabled = json["enabled"].b();
        if (enabled) {
            auto table_key = json.has("table") ? json["table"].s() : endpoint->cache.table;
            auto schema_key = json.has("schema") ? json["schema"].s() : endpoint->cache.schema;
            cache.enabled = true;
            cache.table = table_key;
            cache.schema = schema_key;
        } else {
            cache.enabled = false;
        }
        if (json.has("schedule")) {
            cache.schedule = json["schedule"].s();
        }
        if (json.has("primary-key")) {
            cache.primary_keys.clear();
            for (const auto& pk : json["primary-key"]) {
                cache.primary_keys.push_back(pk.s());
            }
        }
        if (json.has("cursor")) {
            CacheConfig::CursorConfig cursor;
            cursor.column = json["cursor"]["column"].s();
            cursor.type = json["cursor"]["type"].s();
            cache.cursor = cursor;
        }
        if (json.has("rollback-window")) {
            cache.rollback_window = json["rollback-window"].s();
        }
        if (json.has("retention")) {
            const auto& retention = json["retention"];
            if (retention.has("keep-last-snapshots")) {
                cache.retention.keep_last_snapshots = static_cast<std::size_t>(retention["keep-last-snapshots"].i());
            }
            if (retention.has("max-snapshot-age")) {
                cache.retention.max_snapshot_age = retention["max-snapshot-age"].s();
            }
        }
        if (json.has("delete-handling")) {
            cache.delete_handling = json["delete-handling"].s();
        }
        if (json.has("template-file")) {
            cache.template_file = json["template-file"].s();
        }

        return crow::response(200);
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

crow::response TemplateHandler::getCacheTemplate(const crow::request& req, const std::string& path) {
    try {
        auto* endpoint = config_manager_->getEndpointForPath(path);
        if (!endpoint) {
            return crow::response(404, "Endpoint not found");
        }
        if (!endpoint->cache.enabled || !endpoint->cache.template_file) {
            return crow::response(400, "Cache template not configured for this endpoint");
        }

        auto cache_path = resolveTemplatePath(endpoint->cache.template_file.value());
        std::ifstream file(cache_path);
        if (!file.is_open()) {
            return crow::response(500, "Could not open cache template file: " + cache_path.string());
        }

        std::stringstream buffer;
        buffer << file.rdbuf();

        crow::json::wvalue response;
        response["template"] = buffer.str();
        return crow::response(200, response);
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

crow::response TemplateHandler::updateCacheTemplate(const crow::request& req, const std::string& path) {
    try {
        auto json = crow::json::load(req.body);
        if (!json || !json.has("template")) {
            return crow::response(400, "Invalid JSON: missing 'template' field");
        }

        auto* endpoint = config_manager_->getEndpointForPath(path);
        if (!endpoint) {
            return crow::response(404, "Endpoint not found");
        }
        if (!endpoint->cache.enabled || !endpoint->cache.template_file) {
            return crow::response(400, "Cache template not configured for this endpoint");
        }

        auto cache_path = resolveTemplatePath(endpoint->cache.template_file.value());
        std::ofstream file(cache_path);
        if (!file.is_open()) {
            return crow::response(500, "Could not open cache template file for writing: " + cache_path.string());
        }

        file << json["template"].s();
        if (!file.good()) {
            return crow::response(500, "Error writing template to file");
        }

        return crow::response(200);
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

crow::response CacheConfigHandler::refreshCache(const crow::request& req, const std::string& path) {
    try {
        // Find the endpoint
        auto* endpoint = config_manager_->getEndpointForPath(path);
        if (!endpoint) {
            return crow::response(404, "Endpoint not found");
        }

        if (!endpoint->cache.enabled) {
            return crow::response(400, "Cache is not enabled for this endpoint");
        }

        // Get database manager instance
        auto db_manager = DatabaseManager::getInstance();
        auto cache_manager = std::make_shared<CacheManager>(db_manager);

        // Prepare empty parameters map for cache refresh
        std::map<std::string, std::string> params;

        try {
            // Trigger cache refresh
            cache_manager->refreshCache(config_manager_, *endpoint, params);
            return crow::response(200);
        } catch (const std::exception& e) {
            return crow::response(400, std::string("Cache refresh failed: ") + e.what());
        }
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

crow::response CacheConfigHandler::performGarbageCollection(const crow::request& req, const std::string& path) {
    try {
        // Find the endpoint
        auto* endpoint = config_manager_->getEndpointForPath(path);
        if (!endpoint) {
            return crow::response(404, "Endpoint not found");
        }

        if (!endpoint->cache.enabled) {
            return crow::response(400, "Cache is not enabled for this endpoint");
        }

        // Get database manager instance
        auto db_manager = DatabaseManager::getInstance();
        auto cache_manager = std::make_shared<CacheManager>(db_manager);

        try {
            // Trigger garbage collection
            cache_manager->performGarbageCollection(config_manager_, *endpoint, {});
            return crow::response(200, "Garbage collection completed");
        } catch (const std::exception& e) {
            return crow::response(400, std::string("Garbage collection failed: ") + e.what());
        }
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

std::filesystem::path TemplateHandler::resolveTemplatePath(const std::string& source) const {
    std::filesystem::path template_path(source);
    if (template_path.is_absolute()) {
        return template_path;
    }

    if (!config_manager_) {
        return template_path;
    }

    std::filesystem::path base_path(config_manager_->getTemplatePath());
    return (base_path / template_path).lexically_normal();
}

crow::response SchemaHandler::getSchema(const crow::request& req) {
    try {
        CROW_LOG_DEBUG << "SchemaHandler::getSchema called";
        
        // Parse query parameters
        auto url_params = req.url_params;
        bool tables_only = url_params.get("tables") != nullptr;
        bool connections_only = url_params.get("connections") != nullptr;
        bool completion_format = url_params.get("format") && std::string(url_params.get("format")) == "completion";
        std::string specific_connection = url_params.get("connection") ? url_params.get("connection") : "";
        
        CROW_LOG_DEBUG << "Schema query params - tables_only: " << tables_only 
                      << ", connections_only: " << connections_only 
                      << ", completion_format: " << completion_format
                      << ", connection: " << specific_connection;
        
        // Get database manager instance
        auto db_manager = DatabaseManager::getInstance();
        if (!db_manager) {
            CROW_LOG_ERROR << "DatabaseManager instance is null";
            return crow::response(500, "Internal server error: Database manager not initialized");
        }

        // For now, return a simple response based on what's requested
        crow::json::wvalue response;
        
        if (connections_only) {
            // Return connection information
            response["connections"] = crow::json::wvalue::list();
            // TODO: Implement actual connection listing
            return crow::response(200, response);
        }
        
        if (tables_only) {
            // Return table information in the format expected by CLI
            // Use DuckDB's SHOW TABLES command instead of information_schema to avoid parser issues
            const std::string tables_query = R"SQL(
                SELECT 
                    COALESCE(database, 'main') as schema_name,
                    name as table_name,
                    'table' as table_type
                FROM duckdb_tables()
                WHERE database NOT IN ('system', 'temp')
                ORDER BY database, name
            )SQL";
            
            try {
                CROW_LOG_DEBUG << "Executing tables query for schema introspection";
                auto tables_result = db_manager->executeQuery(tables_query, {}, false);
            response["tables"] = crow::json::wvalue::list();
                
                if (tables_result.data.t() != crow::json::type::Null) {
                    CROW_LOG_DEBUG << "Tables query returned " << tables_result.data.size() << " rows";
                    size_t idx = 0;
                    for (size_t i = 0; i < tables_result.data.size(); ++i) {
                        const auto& row = crow::json::load(tables_result.data[i].dump());
                        
                        crow::json::wvalue table_entry;
                        table_entry["name"] = row["table_name"].s();
                        table_entry["schema"] = row["schema_name"].s();
                        table_entry["type"] = row["table_type"].s();
                        std::string qualified_name = std::string(row["schema_name"].s()) + "." + std::string(row["table_name"].s());
                        table_entry["qualified_name"] = qualified_name;
                        
                        response["tables"][idx++] = std::move(table_entry);
                    }
                } else {
                    CROW_LOG_DEBUG << "Tables query returned null result";
                }
            } catch (const std::exception& e) {
                CROW_LOG_ERROR << "Failed to query tables for schema introspection";
                CROW_LOG_ERROR << "Tables query error: " << e.what();
                CROW_LOG_ERROR << "Tables query SQL: " << tables_query;
                // Return empty list on error rather than failing
                response["tables"] = crow::json::wvalue::list();
                response["error"] = std::string("Failed to query tables: ") + e.what();
            }
            
            return crow::response(200, response);
        }

        // Use DuckDB system functions instead of information_schema to avoid parser issues
        // Query tables and columns using duckdb_tables() and duckdb_columns()
        const std::string query = R"SQL(
                SELECT 
                COALESCE(t.database_name, 'main') as schema_name,
                    t.table_name,
                0 as is_view,
                    c.column_name,
                    c.data_type,
                CASE WHEN c.is_nullable THEN 1 ELSE 0 END as is_nullable
            FROM duckdb_tables() t
            LEFT JOIN duckdb_columns() c 
                ON t.database_name = c.database_name
                AND t.schema_name = c.schema_name
                    AND t.table_name = c.table_name
            WHERE COALESCE(t.database_name, 'main') NOT IN ('system', 'temp')
            ORDER BY t.database_name, t.table_name, c.column_index
        )SQL";

        QueryResult result;
        try {
            CROW_LOG_DEBUG << "Executing full schema introspection query";
            result = db_manager->executeQuery(query, {}, false);
            CROW_LOG_DEBUG << "Schema query executed successfully, processing " << result.data.size() << " rows";
        } catch (const std::exception& e) {
            CROW_LOG_ERROR << "========================================";
            CROW_LOG_ERROR << "SCHEMA INTROSPECTION FAILED";
            CROW_LOG_ERROR << "========================================";
            CROW_LOG_ERROR << "Error type: " << typeid(e).name();
            CROW_LOG_ERROR << "Error message: " << e.what();
            CROW_LOG_ERROR << "Query attempted:";
            CROW_LOG_ERROR << query;
            CROW_LOG_ERROR << "========================================";
            // Return empty response with error message for debugging
            response["error"] = std::string("Failed to query schema: ") + e.what();
            response["message"] = "Schema introspection failed. This may be due to DuckDB information_schema limitations or database connection issues.";
            response["query"] = query;
            return crow::response(200, response);
        }
    
        // Process results into the desired JSON structure
        if (result.data.t() == crow::json::type::Null) {
            return crow::response(200, response);
        }

        // Build the tree structure from flat results
        for (size_t i = 0; i < result.data.size(); ++i) {
            const auto& row = crow::json::load(result.data[i].dump());
            
            // Safely extract string values, handling NULL
            auto safe_string = [](const crow::json::rvalue& val) -> std::string {
                if (val.t() == crow::json::type::String) {
                    return val.s();
                }
                return "";
            };
            
            // Safely extract boolean values, handling NULL and integers (0/1)
            auto safe_bool = [](const crow::json::rvalue& val) -> bool {
                if (val.t() == crow::json::type::True) {
                    return true;
                } else if (val.t() == crow::json::type::False) {
                    return false;
                } else if (val.t() == crow::json::type::Number) {
                    return val.i() != 0;
                }
                return false;
            };
            
            const std::string schema_name = safe_string(row["schema_name"]);
            const std::string table_name = safe_string(row["table_name"]);
            const std::string column_name = safe_string(row["column_name"]);
            const std::string data_type = safe_string(row["data_type"]);
            const bool is_nullable = safe_bool(row["is_nullable"]);
            const bool is_view = safe_bool(row["is_view"]);

            // Initialize schema if not exists
            auto schema_names = response.keys();
            if (std::find(schema_names.begin(), schema_names.end(), schema_name) == schema_names.end()) {
                response[schema_name]["tables"] = crow::json::wvalue();
            }

            if (table_name.size() == 0) {
                continue;
            }

            // Initialize table if not exists
            auto table_names = response[schema_name]["tables"].keys();
            if (std::find(table_names.begin(), table_names.end(), table_name) == table_names.end()) {
                response[schema_name]["tables"][table_name]["is_view"] = is_view;
                response[schema_name]["tables"][table_name]["columns"] = crow::json::wvalue();
            }

            if (column_name.size() == 0) {
                continue;
            }

            // Add column info
            auto& columns = response[schema_name]["tables"][table_name]["columns"];
            columns[column_name]["type"] = data_type;
            columns[column_name]["nullable"] = is_nullable;
        }

        // If completion format is requested, transform the response
        if (completion_format) {
            crow::json::wvalue completion_response;
            crow::json::wvalue tables_list = crow::json::wvalue::list();
            crow::json::wvalue columns_list = crow::json::wvalue::list();
            
            // Flatten tables and columns for completion
            for (const auto& schema_name : response.keys()) {
                const auto& schema_obj = response[schema_name];
                auto schema_keys = schema_obj.keys();
                if (std::find(schema_keys.begin(), schema_keys.end(), "tables") != schema_keys.end()) {
                    for (const auto& table_name : schema_obj["tables"].keys()) {
                        const auto& table_obj = schema_obj["tables"][table_name];
                        
                        // Add table entry
                        crow::json::wvalue table_entry;
                        table_entry["name"] = table_name;
                        table_entry["schema"] = schema_name;
                        
                        // Check if it's a view
                        auto table_keys = table_obj.keys();
                        bool is_view = false;
                        if (std::find(table_keys.begin(), table_keys.end(), "is_view") != table_keys.end()) {
                            // Convert to rvalue to access boolean value (or integer 0/1)
                            auto table_json_str = table_obj.dump();
                            auto table_rvalue = crow::json::load(table_json_str);
                            if (table_rvalue && table_rvalue.has("is_view")) {
                                auto& val = table_rvalue["is_view"];
                                if (val.t() == crow::json::type::True) {
                                    is_view = true;
                                } else if (val.t() == crow::json::type::Number) {
                                    is_view = (val.i() != 0);
                                }
                            }
                        }
                        table_entry["type"] = is_view ? "view" : "table";
                        table_entry["qualified_name"] = schema_name + "." + table_name;
                        tables_list[tables_list.size()] = std::move(table_entry);
                        
                        // Add column entries
                        if (std::find(table_keys.begin(), table_keys.end(), "columns") != table_keys.end()) {
                            for (const auto& column_name : table_obj["columns"].keys()) {
                                const auto& column_obj = table_obj["columns"][column_name];
                                
                                crow::json::wvalue column_entry;
                                column_entry["name"] = column_name;
                                column_entry["table"] = table_name;
                                column_entry["schema"] = schema_name;
                                
                                // Extract column type and nullable info
                                auto column_json_str = column_obj.dump();
                                auto column_rvalue = crow::json::load(column_json_str);
                                std::string column_type = "UNKNOWN";
                                bool nullable = true;
                                
                                if (column_rvalue && column_rvalue.has("type")) {
                                    column_type = column_rvalue["type"].s();
                                }
                                if (column_rvalue && column_rvalue.has("nullable")) {
                                    auto& val = column_rvalue["nullable"];
                                    if (val.t() == crow::json::type::True) {
                                        nullable = true;
                                    } else if (val.t() == crow::json::type::False) {
                                        nullable = false;
                                    } else if (val.t() == crow::json::type::Number) {
                                        nullable = (val.i() != 0);
                                    }
                                }
                                
                                column_entry["type"] = column_type;
                                column_entry["nullable"] = nullable;
                                column_entry["qualified_name"] = schema_name + "." + table_name + "." + column_name;
                                columns_list[columns_list.size()] = std::move(column_entry);
                            }
                        }
                    }
                }
            }
            
            completion_response["tables"] = std::move(tables_list);
            completion_response["columns"] = std::move(columns_list);
            return crow::response(200, completion_response);
        }

        return crow::response(200, response);
    } catch (const std::exception& e) {
        CROW_LOG_ERROR << "========================================";
        CROW_LOG_ERROR << "SCHEMA HANDLER EXCEPTION";
        CROW_LOG_ERROR << "========================================";
        CROW_LOG_ERROR << "Error type: " << typeid(e).name();
        CROW_LOG_ERROR << "Error message: " << e.what();
        CROW_LOG_ERROR << "Exception caught in SchemaHandler::getSchema";
        CROW_LOG_ERROR << "========================================";
        return crow::response(500, std::string("Internal server error: ") + e.what());
    } catch (...) {
        CROW_LOG_ERROR << "========================================";
        CROW_LOG_ERROR << "SCHEMA HANDLER UNKNOWN EXCEPTION";
        CROW_LOG_ERROR << "========================================";
        CROW_LOG_ERROR << "Unknown exception type caught in SchemaHandler::getSchema";
        CROW_LOG_ERROR << "========================================";
        return crow::response(500, "Internal server error: Unknown exception in schema handler");
    }
}

crow::response SchemaHandler::refreshSchema(const crow::request& req) {
    try {
        CROW_LOG_INFO << "Schema refresh requested";
        
        // Get database manager instance
        auto db_manager = DatabaseManager::getInstance();
        if (!db_manager) {
            CROW_LOG_ERROR << "DatabaseManager instance is null during schema refresh";
            return crow::response(500, "Internal server error: Database manager not initialized");
        }

        // Re-initialize database connections from config
        CROW_LOG_INFO << "Re-initializing database connections for schema refresh";
        db_manager->initializeDBManagerFromConfig(config_manager_);
        CROW_LOG_INFO << "Schema refresh completed successfully";

        return crow::response(200);
    } catch (const std::exception& e) {
        CROW_LOG_ERROR << "========================================";
        CROW_LOG_ERROR << "SCHEMA REFRESH FAILED";
        CROW_LOG_ERROR << "========================================";
        CROW_LOG_ERROR << "Error type: " << typeid(e).name();
        CROW_LOG_ERROR << "Error message: " << e.what();
        CROW_LOG_ERROR << "Exception caught in SchemaHandler::refreshSchema";
        CROW_LOG_ERROR << "========================================";
        return crow::response(500, std::string("Internal server error: ") + e.what());
    } catch (...) {
        CROW_LOG_ERROR << "========================================";
        CROW_LOG_ERROR << "SCHEMA REFRESH UNKNOWN EXCEPTION";
        CROW_LOG_ERROR << "========================================";
        CROW_LOG_ERROR << "Unknown exception type caught in SchemaHandler::refreshSchema";
        CROW_LOG_ERROR << "========================================";
        return crow::response(500, "Internal server error: Unknown exception in schema refresh");
    }
}




} // namespace flapi
