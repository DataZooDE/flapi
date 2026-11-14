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

/// The GET /api/v1/_config/metrics body (tracing, arrow, endpoints, requests,
/// caches). `filter` = an endpoint path or MCP tool name narrows `requests`
/// and `caches`; empty means everything. Shared with the flapi_get_metrics tool.
crow::json::wvalue buildMetricsJson(const std::shared_ptr<ConfigManager>& config_manager, const std::string& filter = "");

/// Endpoint YAML persistence shared by the REST config service and the flapi_*
/// MCP tools, so a create/update/delete over either survives a reload and a
/// restart the same way. Every write goes through the LOADER's parser
/// (EndpointConfigParser::parseFromFile), so what is served is exactly what a
/// restart would load from the file - mcp-tool, request validators, cache and
/// all - with template paths resolved against the YAML's own directory.
namespace endpoint_files {

struct Outcome {
    bool ok = false;
    int status = 500;          // HTTP status for the REST caller
    std::string error;         // why, when !ok
    EndpointConfig config;     // the parsed endpoint, when ok
    std::string file;          // the YAML written
};

/// Create: `doc` is the endpoint document (the YAML file's content). Written to
/// <template dir>/<slug>.yaml, parsed, validated and added; the file is removed
/// again on any failure. 409 when the endpoint or the file already exists.
Outcome CreateFromDocument(ConfigManager& cm, const YAML::Node& doc);

/// Update: `patch` is deep-merged onto the endpoint's YAML (maps merge,
/// everything else is replaced), written, re-parsed and swapped in; the old
/// file is restored on failure. Keys the patch omits - auth users, validators -
/// are kept, the same PUT semantics as deserializeEndpointConfig(merge).
Outcome UpdateFromPatch(ConfigManager& cm, const EndpointConfig& existing, const YAML::Node& patch);

/// Remove a deleted endpoint's YAML, only when it lies inside the templates
/// directory. The SQL template is left alone. Returns "" or a warning.
std::string RemoveEndpointFile(const ConfigManager& cm, const std::string& file);

/// Why reloadEndpointConfig() failed for an endpoint that exists: the missing
/// file, the parse error or the validation errors.
std::string ReloadFailureReason(const ConfigManager& cm, const EndpointConfig& endpoint);

/// True when the document names its endpoint with the canonical (YAML) keys -
/// url-path / mcp-tool / mcp-resource / mcp-prompt - rather than the legacy
/// camelCase JSON spellings (urlPath, templateSource) only the JSON
/// deserializer understands.
bool IsCanonicalDocument(const YAML::Node& doc);

/// The YAML file name (without extension) for a new endpoint: the url-path
/// segments joined with '_' (/a/b -> a_b), else the MCP name.
std::string FileStemFor(const EndpointConfig& endpoint);

/// A YAML file in the templates directory that declares url-path + method
/// (METHOD upper case), loaded or not; "" when none does.
std::string FileDeclaring(const ConfigManager& cm, const std::string& url_path, const std::string& method);

/// A YAML file in the templates directory that declares the MCP tool,
/// resource or prompt `name`, loaded or not; "" when none does.
std::string FileDeclaringMcpName(const ConfigManager& cm, const std::string& name);

}  // namespace endpoint_files

/// What a config-service slug route (+ optional ?method=) names. Several
/// endpoints may share a url-path (GET /orders + POST /orders): with no
/// method that is a 400 "ambiguous", never an arbitrary pick.
struct SlugTarget {
    bool ok = false;
    bool found = false;          // at least one endpoint matched
    int status = 404;
    std::string error;
    std::string key;             // ConfigManager::EndpointKey() of the target
};
SlugTarget ResolveSlugTarget(const ConfigManager& cm, const std::string& slug, const char* method_param);
/// "%3A" -> ':' in a slug path segment (the CLI's encodeURIComponent).
std::string DecodeSlugSegment(const std::string& slug);

/**
 * Handler for DuckLake audit log operations
 * Responsibility: Retrieve cache synchronization audit logs
 */
class AuditLogHandler {
public:
    explicit AuditLogHandler(std::shared_ptr<ConfigManager> config_manager);
    
    crow::response getCacheAuditLog(const std::string& path);
    /// `table` ("schema.table" or "table"): only that cache table's events,
    /// whichever endpoint sharing it triggered them.
    crow::response getAllCacheAuditLogs(const std::string& table = "");

private:
    std::shared_ptr<ConfigManager> config_manager_;
    std::string buildAuditQuery(const std::string& catalog, const std::string& endpoint_filter = "",
                                const std::string& table_filter = "") const;
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
    // POST /endpoints/reload-file {"file": "..."}: reload or load one YAML.
    crow::response reloadEndpointFile(const crow::request& req);
    crow::response getEndpointParameters(const crow::request& req, const std::string& path);
    crow::response findEndpointsByTemplate(const crow::request& req, const std::string& template_path);
    
    // Helper methods
    crow::json::wvalue endpointConfigToJson(const EndpointConfig& config);
    EndpointConfig jsonToEndpointConfig(const crow::json::rvalue& json, bool merge_existing = false);

private:
    // Shared by the slug- and path-addressed PUT / DELETE.
    crow::response updateExisting(const crow::request& req, const EndpointConfig& existing);
    crow::response removeExisting(const EndpointConfig& existing);

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
    // Below this length the token is still accepted (by REST and MCP alike),
    // but a warning is logged at startup.
    static constexpr std::size_t kMinRecommendedTokenLength = 16;

    explicit ConfigService(std::shared_ptr<ConfigManager> config_manager, 
                          bool enabled = false,
                          const std::string& auth_token = "");

    void registerRoutes(FlapiApp& app);
    void setDocGenerator(std::shared_ptr<OpenAPIDocGenerator> doc_gen);
    
    bool isEnabled() const { return enabled_; }
    bool validateToken(const crow::request& req) const;
    // The bounded route template ("/api/v1/_config/endpoints/<string>/reload")
    // for a config-service request URL; used to label its span.
    static std::string RouteTemplateFor(const std::string& url);

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
