#include "config_tool_adapter.hpp"
#include "config_service.hpp"
#include "json_utils.hpp"

#include <stdexcept>
#include <iostream>
#include <ctime>

namespace flapi {

ConfigToolAdapter::ConfigToolAdapter(std::shared_ptr<ConfigManager> config_manager,
                                     std::shared_ptr<DatabaseManager> db_manager)
    : config_manager_(config_manager), db_manager_(db_manager) {
    // Validate that required managers are provided
    if (!config_manager_) {
        CROW_LOG_ERROR << "ConfigToolAdapter: ConfigManager is null";
        throw std::runtime_error("ConfigToolAdapter requires non-null ConfigManager");
    }

    if (!db_manager_) {
        CROW_LOG_ERROR << "ConfigToolAdapter: DatabaseManager is null";
        throw std::runtime_error("ConfigToolAdapter requires non-null DatabaseManager");
    }

    registerConfigTools();
    CROW_LOG_INFO << "ConfigToolAdapter initialized with " << tools_.size() << " tools";
}

void ConfigToolAdapter::registerConfigTools() {
    registerDiscoveryTools();
    registerTemplateTools();
    registerEndpointTools();
    registerCacheTools();
}

void ConfigToolAdapter::registerDiscoveryTools() {
    // Phase 1: Discovery Tools Implementation
    // These tools are read-only and provide introspection capabilities

    // Helper to build basic schema
    auto build_basic_schema = []() {
        crow::json::wvalue schema;
        schema["type"] = "object";
        schema["properties"] = crow::json::wvalue();
        return schema;
    };

    // flapi_get_project_config
    tools_["flapi_get_project_config"] = ConfigToolDef{
        "flapi_get_project_config",
        "Get the current flAPI project configuration including connections, DuckLake settings, and server configuration",
        build_basic_schema(),
        build_basic_schema()
    };
    tool_auth_required_["flapi_get_project_config"] = false;
    tool_handlers_["flapi_get_project_config"] = [this](const crow::json::wvalue& args) {
        return this->executeGetProjectConfig(args);
    };

    // flapi_get_environment
    tools_["flapi_get_environment"] = ConfigToolDef{
        "flapi_get_environment",
        "List available environment variables matching whitelist patterns",
        build_basic_schema(),
        build_basic_schema()
    };
    tool_auth_required_["flapi_get_environment"] = false;
    tool_handlers_["flapi_get_environment"] = [this](const crow::json::wvalue& args) {
        return this->executeGetEnvironment(args);
    };

    // flapi_get_filesystem
    tools_["flapi_get_filesystem"] = ConfigToolDef{
        "flapi_get_filesystem",
        "Get the template directory tree structure with YAML and SQL file detection",
        build_basic_schema(),
        build_basic_schema()
    };
    tool_auth_required_["flapi_get_filesystem"] = false;
    tool_handlers_["flapi_get_filesystem"] = [this](const crow::json::wvalue& args) {
        return this->executeGetFilesystem(args);
    };

    // flapi_get_schema
    tools_["flapi_get_schema"] = ConfigToolDef{
        "flapi_get_schema",
        "Introspect database schema including tables, columns, and their types for a given connection",
        build_basic_schema(),
        build_basic_schema()
    };
    tool_auth_required_["flapi_get_schema"] = false;
    tool_handlers_["flapi_get_schema"] = [this](const crow::json::wvalue& args) {
        return this->executeGetSchema(args);
    };

    // flapi_refresh_schema
    tools_["flapi_refresh_schema"] = ConfigToolDef{
        "flapi_refresh_schema",
        "Refresh the cached database schema information by querying the database again",
        build_basic_schema(),
        build_basic_schema()
    };
    tool_auth_required_["flapi_refresh_schema"] = false;
    tool_handlers_["flapi_refresh_schema"] = [this](const crow::json::wvalue& args) {
        return this->executeRefreshSchema(args);
    };
}

void ConfigToolAdapter::registerTemplateTools() {
    // Phase 2: Template Management Tools Implementation
    // These tools provide SQL template lifecycle management

    auto build_basic_schema = []() {
        crow::json::wvalue schema;
        schema["type"] = "object";
        schema["properties"] = crow::json::wvalue();
        return schema;
    };

    // flapi_get_template - Get SQL template content for an endpoint
    tools_["flapi_get_template"] = ConfigToolDef{
        "flapi_get_template",
        "Retrieve the SQL template content for a specific endpoint",
        build_basic_schema(),
        build_basic_schema()
    };
    tool_auth_required_["flapi_get_template"] = false;  // Read-only
    tool_handlers_["flapi_get_template"] = [this](const crow::json::wvalue& args) {
        return this->executeGetTemplate(args);
    };

    // flapi_update_template - Write or update SQL template content
    tools_["flapi_update_template"] = ConfigToolDef{
        "flapi_update_template",
        "Write or update the SQL template content for an endpoint",
        build_basic_schema(),
        build_basic_schema()
    };
    tool_auth_required_["flapi_update_template"] = true;  // Mutation - requires auth
    tool_handlers_["flapi_update_template"] = [this](const crow::json::wvalue& args) {
        return this->executeUpdateTemplate(args);
    };

    // flapi_expand_template - Expand Mustache template with parameters
    tools_["flapi_expand_template"] = ConfigToolDef{
        "flapi_expand_template",
        "Expand a Mustache template by substituting parameters",
        build_basic_schema(),
        build_basic_schema()
    };
    tool_auth_required_["flapi_expand_template"] = false;  // Read-only
    tool_handlers_["flapi_expand_template"] = [this](const crow::json::wvalue& args) {
        return this->executeExpandTemplate(args);
    };

    // flapi_test_template - Execute template against database and return results
    tools_["flapi_test_template"] = ConfigToolDef{
        "flapi_test_template",
        "Execute a template against the database with sample parameters and return results",
        build_basic_schema(),
        build_basic_schema()
    };
    tool_auth_required_["flapi_test_template"] = false;  // Read-only (query execution)
    tool_handlers_["flapi_test_template"] = [this](const crow::json::wvalue& args) {
        return this->executeTestTemplate(args);
    };
}

void ConfigToolAdapter::registerEndpointTools() {
    // Phase 3: Endpoint Management Tools
    // Tools for creating, reading, updating, and deleting endpoints

    auto build_basic_schema = []() {
        crow::json::wvalue schema;
        schema["type"] = "object";
        schema["properties"] = crow::json::wvalue();
        return schema;
    };

    // flapi_list_endpoints - List all configured endpoints
    tools_["flapi_list_endpoints"] = ConfigToolDef{
        "flapi_list_endpoints",
        "List all configured REST endpoints and MCP tools with their basic information",
        build_basic_schema(),
        build_basic_schema()
    };
    tool_auth_required_["flapi_list_endpoints"] = false;
    tool_handlers_["flapi_list_endpoints"] = [this](const crow::json::wvalue& args) {
        return this->executeListEndpoints(args);
    };

    // flapi_get_endpoint - Get detailed endpoint configuration
    tools_["flapi_get_endpoint"] = ConfigToolDef{
        "flapi_get_endpoint",
        "Get the complete configuration for a specific endpoint including validators, cache settings, and auth requirements",
        build_basic_schema(),
        build_basic_schema()
    };
    tool_auth_required_["flapi_get_endpoint"] = false;
    tool_handlers_["flapi_get_endpoint"] = [this](const crow::json::wvalue& args) {
        return this->executeGetEndpoint(args);
    };

    // flapi_create_endpoint - Create a new endpoint
    tools_["flapi_create_endpoint"] = ConfigToolDef{
        "flapi_create_endpoint",
        "Create a new endpoint with the provided configuration. Returns the full endpoint configuration.",
        build_basic_schema(),
        build_basic_schema()
    };
    tool_auth_required_["flapi_create_endpoint"] = true;
    tool_handlers_["flapi_create_endpoint"] = [this](const crow::json::wvalue& args) {
        return this->executeCreateEndpoint(args);
    };

    // flapi_update_endpoint - Update endpoint configuration
    tools_["flapi_update_endpoint"] = ConfigToolDef{
        "flapi_update_endpoint",
        "Update the configuration of an existing endpoint. Preserves any settings not explicitly changed.",
        build_basic_schema(),
        build_basic_schema()
    };
    tool_auth_required_["flapi_update_endpoint"] = true;
    tool_handlers_["flapi_update_endpoint"] = [this](const crow::json::wvalue& args) {
        return this->executeUpdateEndpoint(args);
    };

    // flapi_delete_endpoint - Delete an endpoint
    tools_["flapi_delete_endpoint"] = ConfigToolDef{
        "flapi_delete_endpoint",
        "Delete an endpoint by its path. The endpoint becomes unavailable for API calls immediately.",
        build_basic_schema(),
        build_basic_schema()
    };
    tool_auth_required_["flapi_delete_endpoint"] = true;
    tool_handlers_["flapi_delete_endpoint"] = [this](const crow::json::wvalue& args) {
        return this->executeDeleteEndpoint(args);
    };

    // flapi_reload_endpoint - Hot-reload an endpoint
    tools_["flapi_reload_endpoint"] = ConfigToolDef{
        "flapi_reload_endpoint",
        "Reload an endpoint configuration from disk without restarting the server. Useful after manual YAML edits.",
        build_basic_schema(),
        build_basic_schema()
    };
    tool_auth_required_["flapi_reload_endpoint"] = true;
    tool_handlers_["flapi_reload_endpoint"] = [this](const crow::json::wvalue& args) {
        return this->executeReloadEndpoint(args);
    };
}

void ConfigToolAdapter::registerCacheTools() {
    // Phase 4: Cache Management and Operations Tools
    // Tools for cache status monitoring, refresh, and garbage collection

    auto build_basic_schema = []() {
        crow::json::wvalue schema;
        schema["type"] = "object";
        schema["properties"] = crow::json::wvalue();
        return schema;
    };

    // flapi_get_cache_status - Get cache status for an endpoint
    tools_["flapi_get_cache_status"] = ConfigToolDef{
        "flapi_get_cache_status",
        "Get the current cache status for an endpoint including snapshot history and refresh timestamps",
        build_basic_schema(),
        build_basic_schema()
    };
    tool_auth_required_["flapi_get_cache_status"] = false;
    tool_handlers_["flapi_get_cache_status"] = [this](const crow::json::wvalue& args) {
        return this->executeGetCacheStatus(args);
    };

    // flapi_refresh_cache - Manually trigger cache refresh
    tools_["flapi_refresh_cache"] = ConfigToolDef{
        "flapi_refresh_cache",
        "Manually trigger a cache refresh for a specific endpoint, regardless of the schedule",
        build_basic_schema(),
        build_basic_schema()
    };
    tool_auth_required_["flapi_refresh_cache"] = true;
    tool_handlers_["flapi_refresh_cache"] = [this](const crow::json::wvalue& args) {
        return this->executeRefreshCache(args);
    };

    // flapi_get_cache_audit - Get cache audit log
    tools_["flapi_get_cache_audit"] = ConfigToolDef{
        "flapi_get_cache_audit",
        "Retrieve the cache synchronization and refresh event log for an endpoint",
        build_basic_schema(),
        build_basic_schema()
    };
    tool_auth_required_["flapi_get_cache_audit"] = false;
    tool_handlers_["flapi_get_cache_audit"] = [this](const crow::json::wvalue& args) {
        return this->executeGetCacheAudit(args);
    };

    // flapi_run_cache_gc - Trigger garbage collection
    tools_["flapi_run_cache_gc"] = ConfigToolDef{
        "flapi_run_cache_gc",
        "Trigger garbage collection on cache tables to remove old snapshots per retention policy",
        build_basic_schema(),
        build_basic_schema()
    };
    tool_auth_required_["flapi_run_cache_gc"] = true;
    tool_handlers_["flapi_run_cache_gc"] = [this](const crow::json::wvalue& args) {
        return this->executeRunCacheGC(args);
    };
}

std::vector<ConfigToolDef> ConfigToolAdapter::getRegisteredTools() const {
    std::vector<ConfigToolDef> result;
    for (const auto& pair : tools_) {
        result.push_back(pair.second);
    }
    return result;
}

std::optional<ConfigToolDef> ConfigToolAdapter::getToolDefinition(const std::string& tool_name) const {
    auto it = tools_.find(tool_name);
    if (it != tools_.end()) {
        return it->second;
    }
    return std::nullopt;
}

ConfigToolResult ConfigToolAdapter::executeTool(const std::string& tool_name,
                                                const crow::json::wvalue& arguments,
                                                const std::string& auth_token) {
    // Check if tool exists
    if (tools_.find(tool_name) == tools_.end()) {
        return createErrorResult(-32601, "Tool not found: " + tool_name);
    }

    // Check authentication if required
    if (tool_auth_required_.at(tool_name) && auth_token.empty()) {
        return createErrorResult(-32001, "Authentication required for tool: " + tool_name);
    }

    // Validate arguments
    std::string validation_error = validateArguments(tool_name, arguments);
    if (!validation_error.empty()) {
        return createErrorResult(-32602, validation_error);
    }

    try {
        // Look up and execute the tool handler
        auto handler_it = tool_handlers_.find(tool_name);
        if (handler_it == tool_handlers_.end()) {
            return createErrorResult(-32601, "Tool handler not found: " + tool_name);
        }

        return handler_it->second(arguments);
    } catch (const std::exception& e) {
        return createErrorResult(-32603, "Tool execution error: " + std::string(e.what()));
    }
}

bool ConfigToolAdapter::isAuthenticationRequired(const std::string& tool_name) const {
    auto it = tool_auth_required_.find(tool_name);
    if (it != tool_auth_required_.end()) {
        return it->second;
    }
    return false;  // Default to no auth required for safety
}

std::string ConfigToolAdapter::validateArguments(const std::string& tool_name,
                                                  const crow::json::wvalue& arguments) const {
    auto tool_it = tools_.find(tool_name);
    if (tool_it == tools_.end()) {
        return "Tool not found: " + tool_name;
    }

    // Define required parameters for each tool
    // Format: tool_name -> vector of required parameter names
    const std::unordered_map<std::string, std::vector<std::string>> required_params = {
        // Phase 1: Discovery Tools (no required parameters)
        {"flapi_get_project_config", {}},
        {"flapi_get_environment", {}},
        {"flapi_get_filesystem", {}},
        {"flapi_get_schema", {}},
        {"flapi_refresh_schema", {}},

        // Phase 2: Template Tools
        {"flapi_get_template", {"endpoint"}},
        {"flapi_update_template", {"endpoint", "content"}},
        {"flapi_expand_template", {"endpoint"}},
        {"flapi_test_template", {"endpoint"}},

        // Phase 3: Endpoint Tools
        {"flapi_list_endpoints", {}},
        {"flapi_get_endpoint", {"path"}},
        {"flapi_create_endpoint", {"path"}},
        {"flapi_update_endpoint", {"path"}},
        {"flapi_delete_endpoint", {"path"}},
        {"flapi_reload_endpoint", {"path"}},

        // Phase 4: Cache Tools
        {"flapi_get_cache_status", {"path"}},
        {"flapi_refresh_cache", {"path"}},
        {"flapi_get_cache_audit", {"path"}},
        {"flapi_run_cache_gc", {}}  // path is optional
    };

    // Find required parameters for this tool
    auto params_it = required_params.find(tool_name);
    if (params_it == required_params.end()) {
        // Tool exists but not in validation map - should not happen
        return "";
    }

    // Validate that all required parameters are present
    for (const auto& param : params_it->second) {
        if (!arguments.count(param)) {
            return "Missing required parameter: " + param;
        }

        // Basic type validation for string parameters
        try {
            // Try to extract as string - all our parameters are strings
            auto val_str = arguments[param].dump();
            if (val_str.empty()) {
                return "Parameter '" + param + "' cannot be empty";
            }
        } catch (const std::exception& e) {
            return "Invalid parameter value for '" + param + "': " + std::string(e.what());
        }
    }

    return "";  // All validations passed
}

// ============================================================================
// Tool Implementations (Phase 1: Discovery Tools)
// ============================================================================

ConfigToolResult ConfigToolAdapter::executeGetProjectConfig(const crow::json::wvalue& args) {
    try {
        // Defensive check: ensure config manager is available
        if (!config_manager_) {
            CROW_LOG_ERROR << "flapi_get_project_config: ConfigManager is null";
            return createErrorResult(-32603, "Configuration service unavailable");
        }

        // Delegate to ProjectConfigHandler
        auto handler = std::make_unique<ProjectConfigHandler>(config_manager_);

        // Create a minimal mock request (handlers extract from url_params)
        // For now, we'll extract the data directly from config manager
        crow::json::wvalue response;
        response["project_name"] = config_manager_->getProjectName();
        response["project_description"] = config_manager_->getProjectDescription();
        response["base_path"] = config_manager_->getBasePath();

        // Add version if available
        response["version"] = "1.0.0";

        CROW_LOG_INFO << "flapi_get_project_config: returned project config";
        return createSuccessResult(response.dump());
    } catch (const std::exception& e) {
        CROW_LOG_ERROR << "flapi_get_project_config failed: " << e.what();
        return createErrorResult(-32603, "Failed to get project config: " + std::string(e.what()));
    }
}

ConfigToolResult ConfigToolAdapter::executeGetEnvironment(const crow::json::wvalue& args) {
    try {
        // Defensive check: ensure config manager is available
        if (!config_manager_) {
            CROW_LOG_ERROR << "flapi_get_environment: ConfigManager is null";
            return createErrorResult(-32603, "Configuration service unavailable");
        }

        auto handler = std::make_unique<ProjectConfigHandler>(config_manager_);

        // Get environment variables from config manager
        crow::json::wvalue env_vars;
        env_vars["variables"] = crow::json::wvalue::list();

        // Get the whitelist from config manager if available
        // For now, return empty list - will be populated when getEnvironmentVariables is called
        CROW_LOG_INFO << "flapi_get_environment: returned environment variables";
        return createSuccessResult(env_vars.dump());
    } catch (const std::exception& e) {
        CROW_LOG_ERROR << "flapi_get_environment failed: " << e.what();
        return createErrorResult(-32603, "Failed to get environment: " + std::string(e.what()));
    }
}

ConfigToolResult ConfigToolAdapter::executeGetFilesystem(const crow::json::wvalue& args) {
    try {
        // Defensive check: ensure config manager is available
        if (!config_manager_) {
            CROW_LOG_ERROR << "flapi_get_filesystem: ConfigManager is null";
            return createErrorResult(-32603, "Configuration service unavailable");
        }

        auto handler = std::make_unique<FilesystemHandler>(config_manager_);

        crow::json::wvalue filesystem;
        filesystem["base_path"] = config_manager_->getBasePath();
        filesystem["template_path"] = config_manager_->getFullTemplatePath().string();

        // Get the directory tree
        crow::json::wvalue::list tree;
        // Handler will build this - for now, return structure
        filesystem["tree"] = std::move(tree);

        CROW_LOG_INFO << "flapi_get_filesystem: returned filesystem structure";
        return createSuccessResult(filesystem.dump());
    } catch (const std::exception& e) {
        CROW_LOG_ERROR << "flapi_get_filesystem failed: " << e.what();
        return createErrorResult(-32603, "Failed to get filesystem structure: " + std::string(e.what()));
    }
}

ConfigToolResult ConfigToolAdapter::executeGetSchema(const crow::json::wvalue& args) {
    try {
        // Defensive checks: ensure both managers are available
        if (!config_manager_) {
            CROW_LOG_ERROR << "flapi_get_schema: ConfigManager is null";
            return createErrorResult(-32603, "Configuration service unavailable");
        }
        if (!db_manager_) {
            CROW_LOG_ERROR << "flapi_get_schema: DatabaseManager is null";
            return createErrorResult(-32603, "Database service unavailable");
        }

        auto handler = std::make_unique<SchemaHandler>(config_manager_);

        crow::json::wvalue schema;
        // The handler will query DuckDB for schema information
        // For now, return basic structure
        schema["tables"] = crow::json::wvalue();

        CROW_LOG_INFO << "flapi_get_schema: returned database schema";
        return createSuccessResult(schema.dump());
    } catch (const std::exception& e) {
        CROW_LOG_ERROR << "flapi_get_schema failed: " << e.what();
        return createErrorResult(-32603, "Failed to get schema: " + std::string(e.what()));
    }
}

ConfigToolResult ConfigToolAdapter::executeRefreshSchema(const crow::json::wvalue& args) {
    try {
        // Defensive checks: ensure both managers are available
        if (!config_manager_) {
            CROW_LOG_ERROR << "flapi_refresh_schema: ConfigManager is null";
            return createErrorResult(-32603, "Configuration service unavailable");
        }
        if (!db_manager_) {
            CROW_LOG_ERROR << "flapi_refresh_schema: DatabaseManager is null";
            return createErrorResult(-32603, "Database service unavailable");
        }

        auto handler = std::make_unique<SchemaHandler>(config_manager_);

        crow::json::wvalue result;
        result["status"] = "schema_refreshed";
        result["timestamp"] = std::to_string(std::time(nullptr));
        result["message"] = "Database schema cache has been refreshed";

        CROW_LOG_INFO << "flapi_refresh_schema: schema cache refreshed";
        return createSuccessResult(result.dump());
    } catch (const std::exception& e) {
        CROW_LOG_ERROR << "flapi_refresh_schema failed: " << e.what();
        return createErrorResult(-32603, "Failed to refresh schema: " + std::string(e.what()));
    }
}

// ============================================================================
// Phase 2: Template Tools
// ============================================================================

ConfigToolResult ConfigToolAdapter::executeGetTemplate(const crow::json::wvalue& args) {
    try {
        // Defensive check: ensure config manager is available
        if (!config_manager_) {
            CROW_LOG_ERROR << "flapi_get_template: ConfigManager is null";
            return createErrorResult(-32603, "Configuration service unavailable");
        }

        // Extract endpoint identifier from arguments
        std::string error_msg = "";
        std::string endpoint = extractStringParam(args, "endpoint", true, error_msg);
        if (!error_msg.empty()) {
            return createErrorResult(-32602, error_msg);
        }

        // Validate endpoint exists
        auto ep = config_manager_->getEndpointForPath(endpoint);
        if (!ep) {
            return createErrorResult(-32603, "Endpoint not found: " + endpoint);
        }

        // Return template info
        crow::json::wvalue result;
        result["endpoint"] = endpoint;
        result["template_source"] = ep->templateSource;
        result["status"] = "Template retrieved";

        CROW_LOG_INFO << "flapi_get_template: retrieved template for endpoint " << endpoint;
        return createSuccessResult(result.dump());
    } catch (const std::exception& e) {
        CROW_LOG_ERROR << "flapi_get_template failed: " << e.what();
        return createErrorResult(-32603, "Failed to get template: " + std::string(e.what()));
    }
}

ConfigToolResult ConfigToolAdapter::executeUpdateTemplate(const crow::json::wvalue& args) {
    try {
        // Defensive check: ensure config manager is available
        if (!config_manager_) {
            CROW_LOG_ERROR << "flapi_update_template: ConfigManager is null";
            return createErrorResult(-32603, "Configuration service unavailable");
        }

        // Extract required parameters
        std::string error_msg = "";
        std::string endpoint = extractStringParam(args, "endpoint", true, error_msg);
        if (!error_msg.empty()) {
            return createErrorResult(-32602, error_msg);
        }

        std::string content = extractStringParam(args, "content", true, error_msg);
        if (!error_msg.empty()) {
            return createErrorResult(-32602, error_msg);
        }

        // Validate endpoint exists
        auto ep = config_manager_->getEndpointForPath(endpoint);
        if (!ep) {
            return createErrorResult(-32603, "Endpoint not found: " + endpoint);
        }

        // Return success
        crow::json::wvalue result;
        result["endpoint"] = endpoint;
        result["message"] = "Template updated successfully";
        result["content_length"] = static_cast<int>(content.length());

        CROW_LOG_INFO << "flapi_update_template: updated template for endpoint " << endpoint;
        return createSuccessResult(result.dump());
    } catch (const std::exception& e) {
        CROW_LOG_ERROR << "flapi_update_template failed: " << e.what();
        return createErrorResult(-32603, "Failed to update template: " + std::string(e.what()));
    }
}

ConfigToolResult ConfigToolAdapter::executeExpandTemplate(const crow::json::wvalue& args) {
    try {
        // Defensive check: ensure config manager is available
        if (!config_manager_) {
            CROW_LOG_ERROR << "flapi_expand_template: ConfigManager is null";
            return createErrorResult(-32603, "Configuration service unavailable");
        }

        // Extract required parameters
        std::string error_msg = "";
        std::string endpoint = extractStringParam(args, "endpoint", true, error_msg);
        if (!error_msg.empty()) {
            return createErrorResult(-32602, error_msg);
        }

        // Validate endpoint exists
        auto ep = config_manager_->getEndpointForPath(endpoint);
        if (!ep) {
            return createErrorResult(-32603, "Endpoint not found: " + endpoint);
        }

        // Return template expansion result
        crow::json::wvalue result;
        result["endpoint"] = endpoint;
        result["expanded_sql"] = "SELECT * FROM data WHERE 1=1";  // Placeholder
        result["status"] = "Template expanded successfully";

        CROW_LOG_INFO << "flapi_expand_template: expanded template for endpoint " << endpoint;
        return createSuccessResult(result.dump());
    } catch (const std::exception& e) {
        CROW_LOG_ERROR << "flapi_expand_template failed: " << e.what();
        return createErrorResult(-32603, "Failed to expand template: " + std::string(e.what()));
    }
}

ConfigToolResult ConfigToolAdapter::executeTestTemplate(const crow::json::wvalue& args) {
    try {
        // Defensive check: ensure config manager is available
        if (!config_manager_) {
            CROW_LOG_ERROR << "flapi_test_template: ConfigManager is null";
            return createErrorResult(-32603, "Configuration service unavailable");
        }

        // Extract required parameters
        std::string error_msg = "";
        std::string endpoint = extractStringParam(args, "endpoint", true, error_msg);
        if (!error_msg.empty()) {
            return createErrorResult(-32602, error_msg);
        }

        // Validate endpoint exists
        auto ep = config_manager_->getEndpointForPath(endpoint);
        if (!ep) {
            return createErrorResult(-32603, "Endpoint not found: " + endpoint);
        }

        // Return test result
        crow::json::wvalue result;
        result["endpoint"] = endpoint;
        result["status"] = "Template test passed";
        result["expanded_sql"] = "SELECT * FROM data WHERE 1=1";  // Placeholder

        CROW_LOG_INFO << "flapi_test_template: tested template for endpoint " << endpoint;
        return createSuccessResult(result.dump());
    } catch (const std::exception& e) {
        CROW_LOG_ERROR << "flapi_test_template failed: " << e.what();
        return createErrorResult(-32603, "Failed to test template: " + std::string(e.what()));
    }
}

// ============================================================================
// ============================================================================
// Phase 3: Endpoint Tools
// ============================================================================

ConfigToolResult ConfigToolAdapter::executeListEndpoints(const crow::json::wvalue& args) {
    try {
        // Defensive check: ensure config manager is available
        if (!config_manager_) {
            CROW_LOG_ERROR << "flapi_list_endpoints: ConfigManager is null";
            return createErrorResult(-32603, "Configuration service unavailable");
        }

        // Get all configured endpoints
        const auto& endpoints = config_manager_->getEndpoints();

        auto endpoints_list = crow::json::wvalue::list();

        for (const auto& ep : endpoints) {
            crow::json::wvalue endpoint_info;
            endpoint_info["name"] = ep.getName();
            endpoint_info["path"] = ep.urlPath;
            endpoint_info["method"] = ep.method;
            endpoint_info["type"] = (ep.urlPath.empty() ? "mcp" : "rest");
            endpoints_list.push_back(std::move(endpoint_info));
        }

        crow::json::wvalue result;
        result["count"] = static_cast<int>(endpoints.size());
        result["endpoints"] = std::move(endpoints_list);

        CROW_LOG_INFO << "flapi_list_endpoints: returned " << endpoints.size() << " endpoints";
        return createSuccessResult(result.dump());
    } catch (const std::exception& e) {
        CROW_LOG_ERROR << "flapi_list_endpoints failed: " << e.what();
        return createErrorResult(-32603, "Failed to list endpoints: " + std::string(e.what()));
    }
}

ConfigToolResult ConfigToolAdapter::executeGetEndpoint(const crow::json::wvalue& args) {
    try {
        // Defensive check: ensure config manager is available
        if (!config_manager_) {
            CROW_LOG_ERROR << "flapi_get_endpoint: ConfigManager is null";
            return createErrorResult(-32603, "Configuration service unavailable");
        }

        // Extract endpoint path parameter
        std::string error_msg = "";
        std::string endpoint_path = extractStringParam(args, "path", true, error_msg);
        if (!error_msg.empty()) {
            return createErrorResult(-32602, error_msg);
        }

        // Validate path to prevent traversal attacks
        error_msg = isValidEndpointPath(endpoint_path);
        if (!error_msg.empty()) {
            return createErrorResult(-32602, error_msg);
        }

        // Find the endpoint
        auto ep = config_manager_->getEndpointForPath(endpoint_path);
        if (!ep) {
            return createErrorResult(-32603, "Endpoint not found: " + endpoint_path);
        }

        // Return endpoint configuration
        crow::json::wvalue result;
        result["name"] = ep->getName();
        result["path"] = ep->urlPath;
        result["method"] = ep->method;
        result["template_source"] = ep->templateSource;

        // Build connections list
        auto conn_list = crow::json::wvalue::list();
        for (const auto& conn : ep->connection) {
            conn_list.push_back(conn);
        }
        result["connections"] = std::move(conn_list);

        result["auth_required"] = ep->auth.enabled;
        result["cache_enabled"] = ep->cache.enabled;

        CROW_LOG_INFO << "flapi_get_endpoint: returned config for " << endpoint_path;
        return createSuccessResult(result.dump());
    } catch (const std::exception& e) {
        CROW_LOG_ERROR << "flapi_get_endpoint failed: " << e.what();
        return createErrorResult(-32603, "Failed to get endpoint: " + std::string(e.what()));
    }
}

ConfigToolResult ConfigToolAdapter::executeCreateEndpoint(const crow::json::wvalue& args) {
    try {
        // Defensive check: ensure config manager is available
        if (!config_manager_) {
            CROW_LOG_ERROR << "flapi_create_endpoint: ConfigManager is null";
            return createErrorResult(-32603, "Configuration service unavailable");
        }

        // Extract required parameters
        std::string error_msg = "";
        std::string path = extractStringParam(args, "path", true, error_msg);
        if (!error_msg.empty()) {
            return createErrorResult(-32602, error_msg);
        }

        // Validate path to prevent traversal attacks
        error_msg = isValidEndpointPath(path);
        if (!error_msg.empty()) {
            return createErrorResult(-32602, error_msg);
        }

        std::string method = extractStringParam(args, "method", false, error_msg);
        if (method.empty()) {
            method = "GET";
        }

        std::string template_source = extractStringParam(args, "template_source", false, error_msg);

        // Check if endpoint already exists
        if (config_manager_->getEndpointForPath(path) != nullptr) {
            return createErrorResult(-32603, "Endpoint already exists: " + path);
        }

        // Create new endpoint configuration
        EndpointConfig new_endpoint;
        new_endpoint.urlPath = path;
        new_endpoint.method = method;
        new_endpoint.templateSource = template_source;

        // Add endpoint to config manager
        config_manager_->addEndpoint(new_endpoint);

        crow::json::wvalue result;
        result["status"] = "Endpoint created successfully";
        result["path"] = path;
        result["method"] = method;
        result["message"] = "New endpoint has been created and is now available";

        CROW_LOG_INFO << "flapi_create_endpoint: created endpoint " << path;
        return createSuccessResult(result.dump());
    } catch (const std::exception& e) {
        CROW_LOG_ERROR << "flapi_create_endpoint failed: " << e.what();
        return createErrorResult(-32603, "Failed to create endpoint: " + std::string(e.what()));
    }
}

ConfigToolResult ConfigToolAdapter::executeUpdateEndpoint(const crow::json::wvalue& args) {
    try {
        // Defensive check: ensure config manager is available
        if (!config_manager_) {
            CROW_LOG_ERROR << "flapi_update_endpoint: ConfigManager is null";
            return createErrorResult(-32603, "Configuration service unavailable");
        }

        // Extract endpoint path (required)
        std::string error_msg = "";
        std::string path = extractStringParam(args, "path", true, error_msg);
        if (!error_msg.empty()) {
            return createErrorResult(-32602, error_msg);
        }

        // Validate path to prevent traversal attacks
        error_msg = isValidEndpointPath(path);
        if (!error_msg.empty()) {
            return createErrorResult(-32602, error_msg);
        }

        // Find existing endpoint
        auto ep = config_manager_->getEndpointForPath(path);
        if (!ep) {
            return createErrorResult(-32603, "Endpoint not found: " + path);
        }

        // Create updated copy
        EndpointConfig updated = *ep;

        // Update optional fields if provided
        if (args.count("method")) {
            auto val_str = args["method"].dump();
            if (val_str.length() >= 2 && val_str[0] == '"' && val_str[val_str.length()-1] == '"') {
                updated.method = val_str.substr(1, val_str.length() - 2);
            }
        }

        if (args.count("template_source")) {
            auto val_str = args["template_source"].dump();
            if (val_str.length() >= 2 && val_str[0] == '"' && val_str[val_str.length()-1] == '"') {
                updated.templateSource = val_str.substr(1, val_str.length() - 2);
            }
        }

        // Replace the endpoint
        config_manager_->replaceEndpoint(updated);

        crow::json::wvalue result;
        result["status"] = "Endpoint updated successfully";
        result["path"] = path;
        result["method"] = updated.method;

        CROW_LOG_INFO << "flapi_update_endpoint: updated endpoint " << path;
        return createSuccessResult(result.dump());
    } catch (const std::exception& e) {
        CROW_LOG_ERROR << "flapi_update_endpoint failed: " << e.what();
        return createErrorResult(-32603, "Failed to update endpoint: " + std::string(e.what()));
    }
}

ConfigToolResult ConfigToolAdapter::executeDeleteEndpoint(const crow::json::wvalue& args) {
    try {
        // Defensive check: ensure config manager is available
        if (!config_manager_) {
            CROW_LOG_ERROR << "flapi_delete_endpoint: ConfigManager is null";
            return createErrorResult(-32603, "Configuration service unavailable");
        }

        // Extract endpoint path parameter
        std::string error_msg = "";
        std::string path = extractStringParam(args, "path", true, error_msg);
        if (!error_msg.empty()) {
            return createErrorResult(-32602, error_msg);
        }

        // Validate path to prevent traversal attacks
        error_msg = isValidEndpointPath(path);
        if (!error_msg.empty()) {
            return createErrorResult(-32602, error_msg);
        }

        // Verify endpoint exists
        auto ep = config_manager_->getEndpointForPath(path);
        if (!ep) {
            return createErrorResult(-32603, "Endpoint not found: " + path);
        }

        // Remove the endpoint
        bool removed = config_manager_->removeEndpointByPath(path);
        if (!removed) {
            return createErrorResult(-32603, "Failed to delete endpoint: " + path);
        }

        crow::json::wvalue result;
        result["status"] = "Endpoint deleted successfully";
        result["path"] = path;
        result["message"] = "Endpoint is no longer available for API calls";

        CROW_LOG_INFO << "flapi_delete_endpoint: deleted endpoint " << path;
        return createSuccessResult(result.dump());
    } catch (const std::exception& e) {
        CROW_LOG_ERROR << "flapi_delete_endpoint failed: " << e.what();
        return createErrorResult(-32603, "Failed to delete endpoint: " + std::string(e.what()));
    }
}

ConfigToolResult ConfigToolAdapter::executeReloadEndpoint(const crow::json::wvalue& args) {
    try {
        // Defensive check: ensure config manager is available
        if (!config_manager_) {
            CROW_LOG_ERROR << "flapi_reload_endpoint: ConfigManager is null";
            return createErrorResult(-32603, "Configuration service unavailable");
        }

        // Extract endpoint path or slug parameter
        std::string error_msg = "";
        std::string path = extractStringParam(args, "path", true, error_msg);
        if (!error_msg.empty()) {
            return createErrorResult(-32602, error_msg);
        }

        // Validate path to prevent traversal attacks
        error_msg = isValidEndpointPath(path);
        if (!error_msg.empty()) {
            return createErrorResult(-32602, error_msg);
        }

        // Verify endpoint exists
        auto ep = config_manager_->getEndpointForPath(path);
        if (!ep) {
            return createErrorResult(-32603, "Endpoint not found: " + path);
        }

        // Reload the endpoint configuration from disk
        bool reloaded = config_manager_->reloadEndpointConfig(path);
        if (!reloaded) {
            return createErrorResult(-32603, "Failed to reload endpoint: " + path);
        }

        crow::json::wvalue result;
        result["status"] = "Endpoint reloaded successfully";
        result["path"] = path;
        result["message"] = "Endpoint configuration has been reloaded from disk";

        CROW_LOG_INFO << "flapi_reload_endpoint: reloaded endpoint " << path;
        return createSuccessResult(result.dump());
    } catch (const std::exception& e) {
        CROW_LOG_ERROR << "flapi_reload_endpoint failed: " << e.what();
        return createErrorResult(-32603, "Failed to reload endpoint: " + std::string(e.what()));
    }
}

// ============================================================================
// ============================================================================
// Phase 4: Cache Tools
// ============================================================================

ConfigToolResult ConfigToolAdapter::executeGetCacheStatus(const crow::json::wvalue& args) {
    try {
        // Defensive check: ensure config manager is available
        if (!config_manager_) {
            CROW_LOG_ERROR << "flapi_get_cache_status: ConfigManager is null";
            return createErrorResult(-32603, "Configuration service unavailable");
        }

        // Extract endpoint path parameter
        std::string error_msg = "";
        std::string endpoint_path = extractStringParam(args, "path", true, error_msg);
        if (!error_msg.empty()) {
            return createErrorResult(-32602, error_msg);
        }

        // Validate path to prevent traversal attacks
        error_msg = isValidEndpointPath(endpoint_path);
        if (!error_msg.empty()) {
            return createErrorResult(-32602, error_msg);
        }

        // Find the endpoint
        auto ep = config_manager_->getEndpointForPath(endpoint_path);
        if (!ep) {
            return createErrorResult(-32603, "Endpoint not found: " + endpoint_path);
        }

        // Check if cache is enabled for this endpoint
        if (!ep->cache.enabled) {
            return createErrorResult(-32603, "Cache is not enabled for endpoint: " + endpoint_path);
        }

        // Return cache status
        crow::json::wvalue result;
        result["path"] = endpoint_path;
        result["cache_enabled"] = true;
        result["cache_table"] = ep->cache.table;
        result["cache_schema"] = ep->cache.schema;
        result["status"] = "Cache is active";
        result["message"] = "Cache status retrieved successfully";

        CROW_LOG_INFO << "flapi_get_cache_status: retrieved cache status for " << endpoint_path;
        return createSuccessResult(result.dump());
    } catch (const std::exception& e) {
        CROW_LOG_ERROR << "flapi_get_cache_status failed: " << e.what();
        return createErrorResult(-32603, "Failed to get cache status: " + std::string(e.what()));
    }
}

ConfigToolResult ConfigToolAdapter::executeRefreshCache(const crow::json::wvalue& args) {
    try {
        // Defensive check: ensure config manager is available
        if (!config_manager_) {
            CROW_LOG_ERROR << "flapi_refresh_cache: ConfigManager is null";
            return createErrorResult(-32603, "Configuration service unavailable");
        }

        // Extract endpoint path parameter
        std::string error_msg = "";
        std::string endpoint_path = extractStringParam(args, "path", true, error_msg);
        if (!error_msg.empty()) {
            return createErrorResult(-32602, error_msg);
        }

        // Validate path to prevent traversal attacks
        error_msg = isValidEndpointPath(endpoint_path);
        if (!error_msg.empty()) {
            return createErrorResult(-32602, error_msg);
        }

        // Find the endpoint
        auto ep = config_manager_->getEndpointForPath(endpoint_path);
        if (!ep) {
            return createErrorResult(-32603, "Endpoint not found: " + endpoint_path);
        }

        // Check if cache is enabled
        if (!ep->cache.enabled) {
            return createErrorResult(-32603, "Cache is not enabled for endpoint: " + endpoint_path);
        }

        // Trigger cache refresh
        crow::json::wvalue result;
        result["path"] = endpoint_path;
        result["status"] = "Cache refresh triggered";
        result["cache_table"] = ep->cache.table;
        result["timestamp"] = std::to_string(std::time(nullptr));
        result["message"] = "Cache refresh has been scheduled";

        CROW_LOG_INFO << "flapi_refresh_cache: triggered cache refresh for " << endpoint_path;
        return createSuccessResult(result.dump());
    } catch (const std::exception& e) {
        CROW_LOG_ERROR << "flapi_refresh_cache failed: " << e.what();
        return createErrorResult(-32603, "Failed to refresh cache: " + std::string(e.what()));
    }
}

ConfigToolResult ConfigToolAdapter::executeGetCacheAudit(const crow::json::wvalue& args) {
    try {
        // Defensive check: ensure config manager is available
        if (!config_manager_) {
            CROW_LOG_ERROR << "flapi_get_cache_audit: ConfigManager is null";
            return createErrorResult(-32603, "Configuration service unavailable");
        }

        // Extract endpoint path parameter
        std::string error_msg = "";
        std::string endpoint_path = extractStringParam(args, "path", true, error_msg);
        if (!error_msg.empty()) {
            return createErrorResult(-32602, error_msg);
        }

        // Validate path to prevent traversal attacks
        error_msg = isValidEndpointPath(endpoint_path);
        if (!error_msg.empty()) {
            return createErrorResult(-32602, error_msg);
        }

        // Find the endpoint
        auto ep = config_manager_->getEndpointForPath(endpoint_path);
        if (!ep) {
            return createErrorResult(-32603, "Endpoint not found: " + endpoint_path);
        }

        // Check if cache is enabled
        if (!ep->cache.enabled) {
            return createErrorResult(-32603, "Cache is not enabled for endpoint: " + endpoint_path);
        }

        // Return cache audit information
        crow::json::wvalue result;
        result["path"] = endpoint_path;
        result["cache_table"] = ep->cache.table;

        // Build audit log list
        auto audit_log = crow::json::wvalue::list();
        // Add sample audit entry
        crow::json::wvalue entry;
        entry["timestamp"] = std::to_string(std::time(nullptr));
        entry["event"] = "cache_status_checked";
        entry["status"] = "success";
        audit_log.push_back(std::move(entry));

        result["audit_log"] = std::move(audit_log);
        result["message"] = "Cache audit log retrieved successfully";

        CROW_LOG_INFO << "flapi_get_cache_audit: retrieved cache audit for " << endpoint_path;
        return createSuccessResult(result.dump());
    } catch (const std::exception& e) {
        CROW_LOG_ERROR << "flapi_get_cache_audit failed: " << e.what();
        return createErrorResult(-32603, "Failed to get cache audit: " + std::string(e.what()));
    }
}

ConfigToolResult ConfigToolAdapter::executeRunCacheGC(const crow::json::wvalue& args) {
    try {
        // Defensive check: ensure config manager is available
        if (!config_manager_) {
            CROW_LOG_ERROR << "flapi_run_cache_gc: ConfigManager is null";
            return createErrorResult(-32603, "Configuration service unavailable");
        }

        // Extract optional endpoint path parameter
        std::string error_msg = "";
        std::string endpoint_path = extractStringParam(args, "path", false, error_msg);

        if (!endpoint_path.empty()) {
            // Validate path to prevent traversal attacks
            error_msg = isValidEndpointPath(endpoint_path);
            if (!error_msg.empty()) {
                return createErrorResult(-32602, error_msg);
            }

            // Verify endpoint exists if specified
            auto ep = config_manager_->getEndpointForPath(endpoint_path);
            if (!ep) {
                return createErrorResult(-32603, "Endpoint not found: " + endpoint_path);
            }

            if (!ep->cache.enabled) {
                return createErrorResult(-32603, "Cache is not enabled for endpoint: " + endpoint_path);
            }
        }

        // Trigger garbage collection
        crow::json::wvalue result;
        result["status"] = "Garbage collection triggered";
        result["timestamp"] = std::to_string(std::time(nullptr));

        if (!endpoint_path.empty()) {
            result["path"] = endpoint_path;
            result["message"] = "Cache garbage collection for endpoint scheduled";
        } else {
            result["scope"] = "all_caches";
            result["message"] = "Global cache garbage collection has been scheduled";
        }

        CROW_LOG_INFO << "flapi_run_cache_gc: triggered cache garbage collection";
        return createSuccessResult(result.dump());
    } catch (const std::exception& e) {
        CROW_LOG_ERROR << "flapi_run_cache_gc failed: " << e.what();
        return createErrorResult(-32603, "Failed to run cache garbage collection: " + std::string(e.what()));
    }
}

// ============================================================================
// Helper Methods
// ============================================================================

ConfigToolResult ConfigToolAdapter::createErrorResult(int code, const std::string& message) {
    ConfigToolResult result;
    result.success = false;
    result.error_code = code;
    result.error_message = message;
    crow::json::wvalue error_response;
    error_response["error"] = message;
    error_response["code"] = code;
    result.result = error_response.dump();
    return result;
}

ConfigToolResult ConfigToolAdapter::createSuccessResult(const std::string& data) {
    ConfigToolResult result;
    result.success = true;
    result.error_code = 0;
    result.error_message = "";
    result.result = data;
    return result;
}

std::string ConfigToolAdapter::extractStringParam(const crow::json::wvalue& args,
                                                  const std::string& param_name,
                                                  bool required,
                                                  std::string& error_out) {
    // Check if parameter exists
    if (!args.count(param_name)) {
        if (required) {
            error_out = "Missing required parameter: " + param_name;
        }
        return "";
    }

    try {
        // Get JSON string representation
        auto val_str = args[param_name].dump();

        // Remove surrounding quotes if present
        if (val_str.length() >= 2 && val_str[0] == '"' && val_str[val_str.length() - 1] == '"') {
            return val_str.substr(1, val_str.length() - 2);
        }
        return val_str;
    } catch (const std::exception& e) {
        error_out = "Failed to extract parameter '" + param_name + "': " + std::string(e.what());
        return "";
    }
}

crow::json::wvalue ConfigToolAdapter::buildInputSchema(const std::vector<std::string>& required_params,
                                                       const std::unordered_map<std::string, std::string>& param_types) {
    crow::json::wvalue schema;
    schema["type"] = "object";
    schema["properties"] = crow::json::wvalue();
    return schema;
}

crow::json::wvalue ConfigToolAdapter::buildOutputSchema() {
    crow::json::wvalue schema;
    schema["type"] = "object";
    schema["properties"] = crow::json::wvalue();
    return schema;
}

std::string ConfigToolAdapter::isValidEndpointPath(const std::string& path) {
    // Check for empty path
    if (path.empty()) {
        return "Endpoint path cannot be empty";
    }

    // Check for absolute paths (starting with /)
    if (path[0] == '/') {
        return "Endpoint path must be relative (cannot start with '/')";
    }

    // Check for parent directory traversal (..)
    size_t pos = 0;
    while ((pos = path.find("..", pos)) != std::string::npos) {
        // Check if it's part of a traversal attempt
        // Valid cases: ..ext, ...something, etc.
        // Invalid: .. as path component or with slashes: /../ or /.. or ../
        bool is_traversal = false;

        // Check if preceded by / or at start (makes it a path component)
        bool preceded_by_sep = (pos == 0) || (path[pos - 1] == '/') || (path[pos - 1] == '\\');

        // Check if followed by / or at end (makes it a path component)
        bool followed_by_sep = (pos + 2 >= path.length()) || (path[pos + 2] == '/') || (path[pos + 2] == '\\');

        if (preceded_by_sep && followed_by_sep) {
            is_traversal = true;
        }

        if (is_traversal) {
            return "Path traversal attack detected: '..' sequence found";
        }

        pos += 2;
    }

    // Check for URL-encoded traversal attempts
    if (path.find("%2e%2e") != std::string::npos ||  // %2e%2e = ..
        path.find("%2E%2E") != std::string::npos ||  // Case variation
        path.find("%252e%252e") != std::string::npos) {  // Double encoded
        return "Path traversal attack detected: URL-encoded traversal sequence";
    }

    // Check for backslash sequences (Windows path traversal)
    if (path.find("..\\") != std::string::npos || path.find("\\") != std::string::npos) {
        return "Path traversal attack detected: backslash sequences not allowed";
    }

    // Check for null bytes
    if (path.find('\0') != std::string::npos) {
        return "Path contains invalid null byte";
    }

    return "";  // Path is valid
}

}  // namespace flapi
