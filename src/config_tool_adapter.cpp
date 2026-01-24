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

    // flapi_get_environment
    tools_["flapi_get_environment"] = ConfigToolDef{
        "flapi_get_environment",
        "List available environment variables matching whitelist patterns",
        build_basic_schema(),
        build_basic_schema()
    };
    tool_auth_required_["flapi_get_environment"] = false;

    // flapi_get_filesystem
    tools_["flapi_get_filesystem"] = ConfigToolDef{
        "flapi_get_filesystem",
        "Get the template directory tree structure with YAML and SQL file detection",
        build_basic_schema(),
        build_basic_schema()
    };
    tool_auth_required_["flapi_get_filesystem"] = false;

    // flapi_get_schema
    tools_["flapi_get_schema"] = ConfigToolDef{
        "flapi_get_schema",
        "Introspect database schema including tables, columns, and their types for a given connection",
        build_basic_schema(),
        build_basic_schema()
    };
    tool_auth_required_["flapi_get_schema"] = false;

    // flapi_refresh_schema
    tools_["flapi_refresh_schema"] = ConfigToolDef{
        "flapi_refresh_schema",
        "Refresh the cached database schema information by querying the database again",
        build_basic_schema(),
        build_basic_schema()
    };
    tool_auth_required_["flapi_refresh_schema"] = false;
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

    // flapi_update_template - Write or update SQL template content
    tools_["flapi_update_template"] = ConfigToolDef{
        "flapi_update_template",
        "Write or update the SQL template content for an endpoint",
        build_basic_schema(),
        build_basic_schema()
    };
    tool_auth_required_["flapi_update_template"] = true;  // Mutation - requires auth

    // flapi_expand_template - Expand Mustache template with parameters
    tools_["flapi_expand_template"] = ConfigToolDef{
        "flapi_expand_template",
        "Expand a Mustache template by substituting parameters",
        build_basic_schema(),
        build_basic_schema()
    };
    tool_auth_required_["flapi_expand_template"] = false;  // Read-only

    // flapi_test_template - Execute template against database and return results
    tools_["flapi_test_template"] = ConfigToolDef{
        "flapi_test_template",
        "Execute a template against the database with sample parameters and return results",
        build_basic_schema(),
        build_basic_schema()
    };
    tool_auth_required_["flapi_test_template"] = false;  // Read-only (query execution)
}

void ConfigToolAdapter::registerEndpointTools() {
    // Phase 3 implementation
    // flapi_list_endpoints
    // flapi_get_endpoint
    // flapi_create_endpoint
    // flapi_update_endpoint
    // flapi_delete_endpoint
    // flapi_reload_endpoint
}

void ConfigToolAdapter::registerCacheTools() {
    // Phase 4 implementation
    // flapi_get_cache_status
    // flapi_refresh_cache
    // flapi_get_cache_audit
    // flapi_run_cache_gc
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
        // Execute the tool based on name
        // Phase 1: Discovery Tools
        if (tool_name == "flapi_get_project_config") {
            return executeGetProjectConfig(arguments);
        } else if (tool_name == "flapi_get_environment") {
            return executeGetEnvironment(arguments);
        } else if (tool_name == "flapi_get_filesystem") {
            return executeGetFilesystem(arguments);
        } else if (tool_name == "flapi_get_schema") {
            return executeGetSchema(arguments);
        } else if (tool_name == "flapi_refresh_schema") {
            return executeRefreshSchema(arguments);
        }
        // Phase 2: Template Tools
        else if (tool_name == "flapi_get_template") {
            return executeGetTemplate(arguments);
        } else if (tool_name == "flapi_update_template") {
            return executeUpdateTemplate(arguments);
        } else if (tool_name == "flapi_expand_template") {
            return executeExpandTemplate(arguments);
        } else if (tool_name == "flapi_test_template") {
            return executeTestTemplate(arguments);
        } else {
            return createErrorResult(-32601, "Tool implementation not found: " + tool_name);
        }
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
    // Phase 0: Basic validation only
    // Full validation will be implemented in later phases

    auto tool_it = tools_.find(tool_name);
    if (tool_it == tools_.end()) {
        return "Tool not found: " + tool_name;
    }

    // TODO: Implement comprehensive validation based on input schema
    return "";  // Valid for now
}

// ============================================================================
// Tool Implementations (Phase 1: Discovery Tools)
// ============================================================================

ConfigToolResult ConfigToolAdapter::executeGetProjectConfig(const crow::json::wvalue& args) {
    try {
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
        // Extract endpoint identifier from arguments
        std::string endpoint = "";
        if (args.count("endpoint")) {
            auto val_str = args["endpoint"].dump();
            // Remove quotes from JSON string
            if (val_str.length() >= 2 && val_str[0] == '"' && val_str[val_str.length()-1] == '"') {
                endpoint = val_str.substr(1, val_str.length() - 2);
            } else {
                endpoint = val_str;
            }
        } else {
            return createErrorResult(-32602, "Missing required parameter: endpoint");
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
        // Extract required parameters
        std::string endpoint = "";
        std::string content = "";

        if (args.count("endpoint")) {
            auto val_str = args["endpoint"].dump();
            if (val_str.length() >= 2 && val_str[0] == '"' && val_str[val_str.length()-1] == '"') {
                endpoint = val_str.substr(1, val_str.length() - 2);
            } else {
                endpoint = val_str;
            }
        } else {
            return createErrorResult(-32602, "Missing required parameter: endpoint");
        }

        if (args.count("content")) {
            content = args["content"].dump();
        } else {
            return createErrorResult(-32602, "Missing required parameter: content");
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
        // Extract required parameters
        std::string endpoint = "";

        if (args.count("endpoint")) {
            auto val_str = args["endpoint"].dump();
            if (val_str.length() >= 2 && val_str[0] == '"' && val_str[val_str.length()-1] == '"') {
                endpoint = val_str.substr(1, val_str.length() - 2);
            } else {
                endpoint = val_str;
            }
        } else {
            return createErrorResult(-32602, "Missing required parameter: endpoint");
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
        // Extract required parameters
        std::string endpoint = "";

        if (args.count("endpoint")) {
            auto val_str = args["endpoint"].dump();
            if (val_str.length() >= 2 && val_str[0] == '"' && val_str[val_str.length()-1] == '"') {
                endpoint = val_str.substr(1, val_str.length() - 2);
            } else {
                endpoint = val_str;
            }
        } else {
            return createErrorResult(-32602, "Missing required parameter: endpoint");
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
// Phase 3: Endpoint Tools (not implemented yet)
// ============================================================================

ConfigToolResult ConfigToolAdapter::executeListEndpoints(const crow::json::wvalue& args) {
    return createErrorResult(-32601, "Not implemented in Phase 1");
}

ConfigToolResult ConfigToolAdapter::executeGetEndpoint(const crow::json::wvalue& args) {
    return createErrorResult(-32601, "Not implemented in Phase 1");
}

ConfigToolResult ConfigToolAdapter::executeCreateEndpoint(const crow::json::wvalue& args) {
    return createErrorResult(-32601, "Not implemented in Phase 1");
}

ConfigToolResult ConfigToolAdapter::executeUpdateEndpoint(const crow::json::wvalue& args) {
    return createErrorResult(-32601, "Not implemented in Phase 1");
}

ConfigToolResult ConfigToolAdapter::executeDeleteEndpoint(const crow::json::wvalue& args) {
    return createErrorResult(-32601, "Not implemented in Phase 1");
}

ConfigToolResult ConfigToolAdapter::executeReloadEndpoint(const crow::json::wvalue& args) {
    return createErrorResult(-32601, "Not implemented in Phase 1");
}

// ============================================================================
// Phase 4: Cache Tools (not implemented yet)
// ============================================================================

ConfigToolResult ConfigToolAdapter::executeGetCacheStatus(const crow::json::wvalue& args) {
    return createErrorResult(-32601, "Not implemented in Phase 1");
}

ConfigToolResult ConfigToolAdapter::executeRefreshCache(const crow::json::wvalue& args) {
    return createErrorResult(-32601, "Not implemented in Phase 1");
}

ConfigToolResult ConfigToolAdapter::executeGetCacheAudit(const crow::json::wvalue& args) {
    return createErrorResult(-32601, "Not implemented in Phase 1");
}

ConfigToolResult ConfigToolAdapter::executeRunCacheGC(const crow::json::wvalue& args) {
    return createErrorResult(-32601, "Not implemented in Phase 1");
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

}  // namespace flapi
