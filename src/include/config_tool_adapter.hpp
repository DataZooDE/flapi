#pragma once

#include <crow.h>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>

#include "config_manager.hpp"
#include "database_manager.hpp"

namespace flapi {

// Forward declarations
class ConfigManager;
class DatabaseManager;
class ConfigServiceHandler;

/**
 * MCP Tool definition structure
 * Wraps a tool with its input/output schemas
 */
struct ConfigToolDef {
    std::string name;
    std::string description;
    crow::json::wvalue input_schema;
    crow::json::wvalue output_schema;
};

/**
 * Result of a config tool execution
 */
struct ConfigToolResult {
    bool success;
    std::string result;  // JSON string result
    std::string error_message;
    int error_code;  // MCP error code (-32000 to -32099 for server errors)
};

/**
 * ConfigToolAdapter: Bridge between MCP protocol and ConfigService handlers
 *
 * Responsibility:
 * - Register all configuration management tools with the MCP server
 * - Translate MCP tool calls to ConfigService handler invocations
 * - Enforce authentication for all config operations
 * - Map handler errors to MCP error codes
 * - Provide schema information for LLM tool selection
 *
 * Design:
 * - Delegates to existing ConfigService handlers (FilesystemHandler, SchemaHandler, etc.)
 * - Thin adapter layer - minimal logic, maximum reuse of existing handlers
 * - Auto-registered on MCPServer startup
 * - Respects existing Config Service token authentication
 */
class ConfigToolAdapter {
public:
    /**
     * Create a ConfigToolAdapter
     * @param config_manager Shared access to configuration
     * @param db_manager Shared access to database
     */
    ConfigToolAdapter(std::shared_ptr<ConfigManager> config_manager,
                      std::shared_ptr<DatabaseManager> db_manager);

    /**
     * Get all registered config tools
     * @return Vector of tool definitions
     */
    std::vector<ConfigToolDef> getRegisteredTools() const;

    /**
     * Get a specific tool definition by name
     * @param tool_name Name of the tool (e.g., "flapi_get_schema")
     * @return Tool definition if found, empty optional otherwise
     */
    std::optional<ConfigToolDef> getToolDefinition(const std::string& tool_name) const;

    /**
     * Execute a config tool
     * @param tool_name Name of the tool to execute
     * @param arguments Tool arguments as JSON
     * @param auth_token Optional authentication token
     * @return Execution result with success status and data/error
     */
    ConfigToolResult executeTool(const std::string& tool_name,
                                 const crow::json::wvalue& arguments,
                                 const std::string& auth_token = "");

    /**
     * Check if a tool requires authentication
     * @param tool_name Name of the tool
     * @return true if auth is required, false otherwise
     */
    bool isAuthenticationRequired(const std::string& tool_name) const;

    /**
     * Validate tool arguments against input schema
     * @param tool_name Name of the tool
     * @param arguments Arguments to validate
     * @return Error message if validation fails, empty string if valid
     */
    std::string validateArguments(const std::string& tool_name,
                                  const crow::json::wvalue& arguments) const;

private:
    std::shared_ptr<ConfigManager> config_manager_;
    std::shared_ptr<DatabaseManager> db_manager_;
    std::unordered_map<std::string, ConfigToolDef> tools_;
    std::unordered_map<std::string, bool> tool_auth_required_;

    // Tool registration
    void registerConfigTools();
    void registerDiscoveryTools();
    void registerTemplateTools();
    void registerEndpointTools();
    void registerCacheTools();

    // Tool implementations (delegating to handlers)
    ConfigToolResult executeGetProjectConfig(const crow::json::wvalue& args);
    ConfigToolResult executeGetEnvironment(const crow::json::wvalue& args);
    ConfigToolResult executeGetFilesystem(const crow::json::wvalue& args);
    ConfigToolResult executeGetSchema(const crow::json::wvalue& args);
    ConfigToolResult executeRefreshSchema(const crow::json::wvalue& args);

    ConfigToolResult executeGetTemplate(const crow::json::wvalue& args);
    ConfigToolResult executeUpdateTemplate(const crow::json::wvalue& args);
    ConfigToolResult executeExpandTemplate(const crow::json::wvalue& args);
    ConfigToolResult executeTestTemplate(const crow::json::wvalue& args);

    ConfigToolResult executeListEndpoints(const crow::json::wvalue& args);
    ConfigToolResult executeGetEndpoint(const crow::json::wvalue& args);
    ConfigToolResult executeCreateEndpoint(const crow::json::wvalue& args);
    ConfigToolResult executeUpdateEndpoint(const crow::json::wvalue& args);
    ConfigToolResult executeDeleteEndpoint(const crow::json::wvalue& args);
    ConfigToolResult executeReloadEndpoint(const crow::json::wvalue& args);

    ConfigToolResult executeGetCacheStatus(const crow::json::wvalue& args);
    ConfigToolResult executeRefreshCache(const crow::json::wvalue& args);
    ConfigToolResult executeGetCacheAudit(const crow::json::wvalue& args);
    ConfigToolResult executeRunCacheGC(const crow::json::wvalue& args);

    // Helper methods
    ConfigToolResult createErrorResult(int code, const std::string& message);
    ConfigToolResult createSuccessResult(const std::string& data);
    crow::json::wvalue buildInputSchema(const std::vector<std::string>& required_params,
                                       const std::unordered_map<std::string, std::string>& param_types);
    crow::json::wvalue buildOutputSchema();
};

}  // namespace flapi
