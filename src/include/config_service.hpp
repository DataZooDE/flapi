#pragma once

#include <crow.h>
#include "crow/middlewares/cors.h"
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>

#include "api_server.hpp"
#include "config_manager.hpp"
#include "request_handler.hpp"
#include "open_api_doc_generator.hpp"

namespace flapi {

// Forward declarations
class AuditLogHandler;
class FilesystemHandler;
class SchemaHandler;
class TemplateHandler;
class CacheConfigHandler;
class EndpointConfigHandler;
class ProjectConfigHandler;
class OpenAPIDocGenerator;

/**
 * Handler for DuckLake audit log operations
 * Responsibility: Retrieve cache synchronization audit logs
 */
class AuditLogHandler {
public:
    explicit AuditLogHandler(std::shared_ptr<ConfigManager> config_manager);
    
    crow::response getCacheAuditLog(const std::string& path);
    crow::response getAllCacheAuditLogs();

private:
    std::shared_ptr<ConfigManager> config_manager_;
    std::string buildAuditQuery(const std::string& catalog, const std::string& endpoint_filter = "") const;
};

/**
 * Handler for filesystem structure operations
 * Responsibility: Build and return filesystem tree structure
 */
class FilesystemHandler {
public:
    explicit FilesystemHandler(std::shared_ptr<ConfigManager> config_manager);
    
    crow::response getFilesystemStructure(const crow::request& req);

private:
    std::shared_ptr<ConfigManager> config_manager_;
    
    void buildDirectoryTree(const std::filesystem::path& root_path,
                           const std::filesystem::path& current_path,
                           crow::json::wvalue::list& tree);
    
    crow::json::wvalue buildFileNode(const std::filesystem::path& file_path,
                                      const std::filesystem::path& root_path);
};

/**
 * Handler for database schema operations
 * Responsibility: Database introspection and schema retrieval
 */
class SchemaHandler {
public:
    explicit SchemaHandler(std::shared_ptr<ConfigManager> config_manager);
    
    crow::response getSchema(const crow::request& req);
    crow::response refreshSchema(const crow::request& req);

private:
    std::shared_ptr<ConfigManager> config_manager_;
};

/**
 * Handler for template operations
 * Responsibility: Template CRUD, expansion, validation, and testing
 */
class TemplateHandler {
public:
    explicit TemplateHandler(std::shared_ptr<ConfigManager> config_manager);
    
    // Slug-based methods (works for both REST and MCP)
    crow::response getEndpointTemplateBySlug(const crow::request& req, const std::string& slug);
    crow::response updateEndpointTemplateBySlug(const crow::request& req, const std::string& slug);
    crow::response expandTemplateBySlug(const crow::request& req, const std::string& slug);
    crow::response testTemplateBySlug(const crow::request& req, const std::string& slug);
    
    // Legacy path-based methods (deprecated)
    crow::response getEndpointTemplate(const crow::request& req, const std::string& path);
    crow::response updateEndpointTemplate(const crow::request& req, const std::string& path);
    crow::response expandTemplate(const crow::request& req, const std::string& path);
    crow::response testTemplate(const crow::request& req, const std::string& path);
    crow::response getCacheTemplate(const crow::request& req, const std::string& path);
    crow::response updateCacheTemplate(const crow::request& req, const std::string& path);

private:
    std::shared_ptr<ConfigManager> config_manager_;
    std::filesystem::path resolveTemplatePath(const std::string& source) const;
};

/**
 * Handler for cache configuration operations
 * Responsibility: Cache config CRUD and cache operations (refresh, GC)
 */
class CacheConfigHandler {
public:
    explicit CacheConfigHandler(std::shared_ptr<ConfigManager> config_manager);
    
    crow::response getCacheConfig(const crow::request& req, const std::string& path);
    crow::response updateCacheConfig(const crow::request& req, const std::string& path);
    crow::response refreshCache(const crow::request& req, const std::string& path);
    crow::response performGarbageCollection(const crow::request& req, const std::string& path);

private:
    std::shared_ptr<ConfigManager> config_manager_;
};

/**
 * Handler for endpoint configuration operations
 * Responsibility: Endpoint CRUD, validation, and reload
 */
class EndpointConfigHandler {
public:
    explicit EndpointConfigHandler(std::shared_ptr<ConfigManager> config_manager);
    
    crow::response listEndpoints(const crow::request& req);
    crow::response createEndpoint(const crow::request& req);
    
    // Centralized slug-based lookups (works for both REST and MCP)
    crow::response getEndpointConfigBySlug(const crow::request& req, const std::string& slug);
    crow::response updateEndpointConfigBySlug(const crow::request& req, const std::string& slug);
    crow::response deleteEndpointBySlug(const crow::request& req, const std::string& slug);
    
    // Legacy path-based methods (deprecated, use slug-based instead)
    crow::response getEndpointConfig(const crow::request& req, const std::string& path);
    crow::response updateEndpointConfig(const crow::request& req, const std::string& path);
    crow::response deleteEndpoint(const crow::request& req, const std::string& path);
    
    crow::response validateEndpointConfig(const crow::request& req, const std::string& path);
    crow::response reloadEndpointConfig(const crow::request& req, const std::string& path);
    crow::response getEndpointParameters(const crow::request& req, const std::string& path);
    crow::response findEndpointsByTemplate(const crow::request& req, const std::string& template_path);
    
    // Helper methods
    crow::json::wvalue endpointConfigToJson(const EndpointConfig& config);
    EndpointConfig jsonToEndpointConfig(const crow::json::rvalue& json);

private:
    std::shared_ptr<ConfigManager> config_manager_;
};

/**
 * Handler for project-level configuration
 * Responsibility: Project config retrieval and updates
 */
class ProjectConfigHandler {
public:
    explicit ProjectConfigHandler(std::shared_ptr<ConfigManager> config_manager);
    
    crow::response getProjectConfig(const crow::request& req);
    crow::response updateProjectConfig(const crow::request& req);
    crow::response getEnvironmentVariables(const crow::request& req);

private:
    std::shared_ptr<ConfigManager> config_manager_;
};

/**
 * Main ConfigService - acts as a facade coordinating all handlers
 * Responsibility: Route registration and delegation to specialized handlers
 */
class ConfigService {
public:
    explicit ConfigService(std::shared_ptr<ConfigManager> config_manager, 
                          bool enabled = false,
                          const std::string& auth_token = "");

    void registerRoutes(FlapiApp& app);
    void setDocGenerator(std::shared_ptr<OpenAPIDocGenerator> doc_gen);
    
    bool isEnabled() const { return enabled_; }
    bool validateToken(const crow::request& req) const;

    std::shared_ptr<ConfigManager> config_manager;
    
    // Public delegation methods for backward compatibility (used by tests)
    crow::response getFilesystemStructure(const crow::request& req) {
        return filesystem_handler_->getFilesystemStructure(req);
    }
    
    crow::response getCacheConfig(const crow::request& req, const std::string& path) {
        return cache_handler_->getCacheConfig(req, path);
    }

private:
    // Handler instances
    std::unique_ptr<AuditLogHandler> audit_handler_;
    std::unique_ptr<FilesystemHandler> filesystem_handler_;
    std::unique_ptr<SchemaHandler> schema_handler_;
    std::unique_ptr<TemplateHandler> template_handler_;
    std::unique_ptr<CacheConfigHandler> cache_handler_;
    std::unique_ptr<EndpointConfigHandler> endpoint_handler_;
    std::unique_ptr<ProjectConfigHandler> project_handler_;
    
    // OpenAPI documentation generator
    std::shared_ptr<OpenAPIDocGenerator> doc_generator_;
    
    // Authentication
    bool enabled_;
    std::string auth_token_;
    
    // Runtime configuration
    std::string current_log_level_ = "info";
};

} // namespace flapi
