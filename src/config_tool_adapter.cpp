#include "config_tool_adapter.hpp"

#include <openssl/crypto.h>
#include "config_service.hpp"
#include "json_utils.hpp"
#include "path_utils.hpp"
#include "template_secrets.hpp"

#include <stdexcept>
#include <iostream>
#include <ctime>

namespace flapi {

ConfigToolAdapter::ConfigToolAdapter(std::shared_ptr<ConfigManager> config_manager,
                                     std::shared_ptr<DatabaseManager> db_manager,
                                     std::string expected_auth_token)
    : expected_auth_token_(std::move(expected_auth_token)),
      config_manager_(config_manager), db_manager_(db_manager) {
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

namespace {

// The input schema an agent acts on.
//
// Every tool used to ship `{"type":"object","properties":{}}` while a separate
// table imposed required arguments at call time - so tools/list declared no
// parameters, an agent called with `{}`, and got -32602. Now that the tools do
// real work that is a confidently wrong answer of its own, and it lets the
// advertised schema drift from the validation that actually runs.
struct SchemaField {
    const char* name;
    const char* type;
    const char* description;
    bool required;
};

const SchemaField kPath{"path", "string",
                        "The endpoint's url-path, e.g. \"/customers/\".", true};
const SchemaField kEndpoint{"endpoint", "string",
                            "The endpoint's url-path, e.g. \"/customers/\".", true};
const SchemaField kContent{"content", "string", "The full SQL template text.", true};
const SchemaField kParams{"params", "object",
                          "Template parameters, as a name/value object.", false};

crow::json::wvalue build_basic_schema() {
    crow::json::wvalue schema;
    schema["type"] = "object";
    schema["properties"] = crow::json::wvalue::object();
    return schema;
}

crow::json::wvalue build_schema(std::initializer_list<SchemaField> fields) {
    crow::json::wvalue schema;
    schema["type"] = "object";
    crow::json::wvalue properties = crow::json::wvalue::object();
    crow::json::wvalue::list required;
    for (const auto& field : fields) {
        crow::json::wvalue property;
        property["type"] = field.type;
        property["description"] = field.description;
        properties[field.name] = std::move(property);
        if (field.required) {
            required.push_back(std::string(field.name));
        }
    }
    schema["properties"] = std::move(properties);
    if (!required.empty()) {
        schema["required"] = std::move(required);
    }
    return schema;
}

}  // namespace

void ConfigToolAdapter::registerConfigTools() {
    registerDiscoveryTools();
    registerTemplateTools();
    registerEndpointTools();
    registerCacheTools();
}

void ConfigToolAdapter::registerDiscoveryTools() {
    // Phase 1: Discovery Tools Implementation
    // These tools are read-only and provide introspection capabilities

    // Helper to build basic schema with empty properties object

    // flapi_get_project_config
    tools_["flapi_get_project_config"] = ConfigToolDef{
        "flapi_get_project_config",
        "Get the current flAPI project configuration including connections, DuckLake settings, and server configuration",
        build_basic_schema(),
        build_basic_schema()
    };
    // EVERY flapi_* tool requires the config-service token.
    //
    // These are the config service's own operations, and every equivalent
    // REST route is behind validateToken. The MCP side used to mark twelve of
    // them auth_required=false as "read-only discovery" - which was survivable
    // only while their bodies returned hardcoded data. Once they were wired to
    // the real handlers, that classification became three unauthenticated
    // disclosures at once:
    //
    //   flapi_get_environment  -> the VALUES of every whitelisted environment
    //                             variable, the same class TemplateSecrets
    //                             exists to redact;
    //   flapi_expand_template  -> the rendered SQL, i.e. whatever the template
    //                             interpolated from conn.* and env.*, through
    //                             a path needing no _dryRun flag;
    //   flapi_test_template    -> EXECUTION of any endpoint's template with
    //                             caller-supplied parameters, including
    //                             endpoints behind an auth: block and
    //                             endpoints not exposed as MCP tools at all.
    //
    // "Read-only" was never the right axis: reading a secret is a disclosure,
    // and reading rows is what an endpoint's auth exists to control. Parity
    // with the REST routes is the rule, and tokenMatchesConfigured fails
    // closed - so with no config-service token configured these tools do
    // nothing, which is correct: the config service is off.
    tool_auth_required_["flapi_get_project_config"] = true;
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
    tool_auth_required_["flapi_get_environment"] = true;
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
    tool_auth_required_["flapi_get_filesystem"] = true;
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
    tool_auth_required_["flapi_get_schema"] = true;
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
    tool_auth_required_["flapi_refresh_schema"] = true;
    tool_handlers_["flapi_refresh_schema"] = [this](const crow::json::wvalue& args) {
        return this->executeRefreshSchema(args);
    };
}

void ConfigToolAdapter::registerTemplateTools() {
    // Phase 2: Template Management Tools Implementation
    // These tools provide SQL template lifecycle management

    // flapi_get_template - Get SQL template content for an endpoint
    tools_["flapi_get_template"] = ConfigToolDef{
        "flapi_get_template",
        "Retrieve the SQL template content for a specific endpoint",
        build_schema({kEndpoint}),
        build_basic_schema()
    };
    tool_auth_required_["flapi_get_template"] = true;
    tool_handlers_["flapi_get_template"] = [this](const crow::json::wvalue& args) {
        return this->executeGetTemplate(args);
    };

    // flapi_update_template - Write or update SQL template content
    tools_["flapi_update_template"] = ConfigToolDef{
        "flapi_update_template",
        "Write or update the SQL template content for an endpoint",
        build_schema({kEndpoint, kContent}),
        build_basic_schema()
    };
    tool_auth_required_["flapi_update_template"] = true;
    tool_handlers_["flapi_update_template"] = [this](const crow::json::wvalue& args) {
        return this->executeUpdateTemplate(args);
    };

    // flapi_expand_template - Expand Mustache template with parameters
    tools_["flapi_expand_template"] = ConfigToolDef{
        "flapi_expand_template",
        "Expand a Mustache template by substituting parameters",
        build_schema({kEndpoint, kParams}),
        build_basic_schema()
    };
    tool_auth_required_["flapi_expand_template"] = true;
    tool_handlers_["flapi_expand_template"] = [this](const crow::json::wvalue& args) {
        return this->executeExpandTemplate(args);
    };

    // flapi_test_template - Execute template against database and return results
    tools_["flapi_test_template"] = ConfigToolDef{
        "flapi_test_template",
        "Execute a template against the database with sample parameters and return results",
        build_schema({kEndpoint, kParams}),
        build_basic_schema()
    };
    tool_auth_required_["flapi_test_template"] = true;
    tool_handlers_["flapi_test_template"] = [this](const crow::json::wvalue& args) {
        return this->executeTestTemplate(args);
    };
}

void ConfigToolAdapter::registerEndpointTools() {
    // Phase 3: Endpoint Management Tools
    // Tools for creating, reading, updating, and deleting endpoints

    // flapi_list_endpoints - List all configured endpoints
    tools_["flapi_list_endpoints"] = ConfigToolDef{
        "flapi_list_endpoints",
        "List all configured REST endpoints and MCP tools with their basic information",
        build_basic_schema(),
        build_basic_schema()
    };
    tool_auth_required_["flapi_list_endpoints"] = true;
    tool_handlers_["flapi_list_endpoints"] = [this](const crow::json::wvalue& args) {
        return this->executeListEndpoints(args);
    };

    // flapi_get_endpoint - Get detailed endpoint configuration
    tools_["flapi_get_endpoint"] = ConfigToolDef{
        "flapi_get_endpoint",
        "Get the complete configuration for a specific endpoint including validators, cache settings, and auth requirements",
        build_schema({kPath}),
        build_basic_schema()
    };
    tool_auth_required_["flapi_get_endpoint"] = true;
    tool_handlers_["flapi_get_endpoint"] = [this](const crow::json::wvalue& args) {
        return this->executeGetEndpoint(args);
    };

    // flapi_create_endpoint - Create a new endpoint
    tools_["flapi_create_endpoint"] = ConfigToolDef{
        "flapi_create_endpoint",
        "Create a new endpoint with the provided configuration. Returns the full endpoint configuration.",
        build_schema({kPath}),
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
        build_schema({kPath}),
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
        build_schema({kPath}),
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
        build_schema({kPath}),
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

    // flapi_get_cache_status - Get cache status for an endpoint
    tools_["flapi_get_cache_status"] = ConfigToolDef{
        "flapi_get_cache_status",
        "Get an endpoint's cache configuration: whether caching is enabled, the cache table and schema, the refresh schedule, cursor and retention policy. For refresh history use flapi_get_cache_audit.",
        build_schema({kPath}),
        build_basic_schema()
    };
    tool_auth_required_["flapi_get_cache_status"] = true;
    tool_handlers_["flapi_get_cache_status"] = [this](const crow::json::wvalue& args) {
        return this->executeGetCacheStatus(args);
    };

    // flapi_refresh_cache - Manually trigger cache refresh
    tools_["flapi_refresh_cache"] = ConfigToolDef{
        "flapi_refresh_cache",
        "Manually trigger a cache refresh for a specific endpoint, regardless of the schedule",
        build_schema({kPath}),
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
        build_schema({kPath}),
        build_basic_schema()
    };
    tool_auth_required_["flapi_get_cache_audit"] = true;
    tool_handlers_["flapi_get_cache_audit"] = [this](const crow::json::wvalue& args) {
        return this->executeGetCacheAudit(args);
    };

    // flapi_run_cache_gc - Trigger garbage collection
    tools_["flapi_run_cache_gc"] = ConfigToolDef{
        "flapi_run_cache_gc",
        "Expire one endpoint's old cache snapshots according to its retention policy. `path` is required; there is no all-endpoints form.",
        build_schema({kPath}),
        build_basic_schema()
    };
    tool_auth_required_["flapi_run_cache_gc"] = true;
    tool_handlers_["flapi_run_cache_gc"] = [this](const crow::json::wvalue& args) {
        return this->executeRunCacheGC(args);
    };
}

ConfigToolResult ConfigToolAdapter::fromHandler(const std::string& tool_name,
                                                const crow::response& response,
                                                const TemplateSecrets* secrets) {
    if (response.code >= 200 && response.code < 300) {
        // A handler that succeeds with no body (refreshCache returns a bare
        // 200) would otherwise hand the agent an empty string, which it
        // cannot tell from a broken tool.
        if (response.body.empty()) {
            crow::json::wvalue ok;
            ok["status"] = "ok";
            ok["tool"] = tool_name;
            return ConfigToolResult{true, ok.dump(), "", 0};
        }
        return ConfigToolResult{true, response.body, "", 0};
    }

    // Scrubbed HERE, once, rather than at each of the nineteen call sites.
    // A handler's failure body can quote the statement that failed - the
    // rendered template - so this is the same invariant as the REST catch
    // blocks, MCP tools/call and resources/read. Putting it at the boundary
    // is what stops the next delegated tool from being a new copy of it.
    std::string detail = response.body.empty()
                             ? ("Request failed with status " + std::to_string(response.code))
                             : response.body;

    // No secret set means the caller could not name an endpoint, so nothing
    // here can be proven safe to quote. Opaque is the correct default: the
    // scrub used to be opt-in, and most delegated tools passed nothing, so
    // "centralised at the boundary" was true of the code and false of the
    // behaviour.
    if (secrets == nullptr) {
        CROW_LOG_WARNING << tool_name << " failed: " << response.code << " " << detail;
        const int opaque_code = (response.code == 404 || response.code == 400) ? -32602 : -32603;
        // A 4xx names what the CALLER got wrong and cannot quote a rendered
        // template, so it stays actionable.
        if (response.code >= 400 && response.code < 500) {
            return ConfigToolResult{false, "", detail, opaque_code};
        }
        return ConfigToolResult{false, "",
                                tool_name + " failed; see the server log for details.",
                                opaque_code};
    }
    // Scrubbed before logging as well as before returning.
    detail = secrets->withhold()
                 ? std::string("the server could not describe this failure without "
                               "risking disclosure")
                 : secrets->scrub(std::move(detail));
    CROW_LOG_WARNING << tool_name << " failed: " << response.code << " " << detail;

    // 404 is the caller naming something that does not exist - an invalid
    // params error, not a server error.
    const int code = (response.code == 404 || response.code == 400) ? -32602 : -32603;
    return ConfigToolResult{false, "", detail, code};
}

crow::request ConfigToolAdapter::handlerRequest(const std::string& body) {
    crow::request req;
    req.body = body;
    return req;
}

std::vector<ConfigToolDef> ConfigToolAdapter::getRegisteredTools() const {
    std::vector<ConfigToolDef> result;
    for (const auto& pair : tools_) {
        // tools/list is a contract: an agent that sees a tool will call it.
        // A tool with no handler cannot honour that, so it is not offered -
        // which is the structural form of "no tool fabricates a result", the
        // defect eleven of these had.
        if (tool_handlers_.count(pair.first) == 0) {
            CROW_LOG_ERROR << "config tool '" << pair.first
                           << "' is registered with no handler and will not be "
                              "advertised; this is a programming error";
            continue;
        }
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
    if (tool_auth_required_.at(tool_name)) {
        if (auth_token.empty()) {
            return createErrorResult(-32001, "Authentication required for tool: " + tool_name);
        }

        // Format first - a cheap, non-secret-dependent reject for obviously
        // malformed input.
        std::string token_error = validateAuthToken(auth_token);
        if (token_error.empty() && !tokenMatchesConfigured(auth_token)) {
            // This is the check that was missing. validateAuthToken only ever
            // inspected the SHAPE of the token - its length, its character set
            // and its scheme name - and returned success for anything
            // well-formed. The configured config-service token was never
            // compared, so `Authorization: Bearer anything-at-all` satisfied
            // every tool declared auth_required.
            token_error = "Invalid authentication token";
        }
        if (!token_error.empty()) {
            CROW_LOG_WARNING << "Tool execution denied - auth validation failed for " << tool_name << ": " << token_error;
            return createErrorResult(-32001, "Authentication validation failed: " + token_error);
        }
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
        {"flapi_run_cache_gc", {"path"}}
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
    // Delegated. The hand-rolled version reported version "1.0.0" for every
    // build and omitted most of what its description promised.
    if (!config_manager_) {
        return createErrorResult(-32603, "Configuration service unavailable");
    }
    ProjectConfigHandler handler(config_manager_);
    return fromHandler("flapi_get_project_config",
                       handler.getProjectConfig(handlerRequest()));
}

ConfigToolResult ConfigToolAdapter::executeGetEnvironment(const crow::json::wvalue& args) {
    // Delegated. This used to build `{"variables": []}` and log "returned
    // environment variables" - an empty list indistinguishable from a
    // deployment that whitelists nothing, returned as a success.
    if (!config_manager_) {
        return createErrorResult(-32603, "Configuration service unavailable");
    }
    ProjectConfigHandler handler(config_manager_);
    return fromHandler("flapi_get_environment",
                       handler.getEnvironmentVariables(handlerRequest()));
}

ConfigToolResult ConfigToolAdapter::executeGetFilesystem(const crow::json::wvalue& args) {
    // Delegated. The `tree` was always an empty list, under the comment
    // "Handler will build this - for now, return structure".
    if (!config_manager_) {
        return createErrorResult(-32603, "Configuration service unavailable");
    }
    FilesystemHandler handler(config_manager_);
    return fromHandler("flapi_get_filesystem",
                       handler.getFilesystemStructure(handlerRequest()));
}

ConfigToolResult ConfigToolAdapter::executeGetSchema(const crow::json::wvalue& args) {
    // Delegated. This returned `{"tables": null}` for every catalog.
    if (!config_manager_) {
        return createErrorResult(-32603, "Configuration service unavailable");
    }
    if (!db_manager_) {
        return createErrorResult(-32603, "Database service unavailable");
    }
    SchemaHandler handler(config_manager_);
    return fromHandler("flapi_get_schema", handler.getSchema(handlerRequest()));
}

ConfigToolResult ConfigToolAdapter::executeRefreshSchema(const crow::json::wvalue& args) {
    // Delegated. This constructed a SchemaHandler, discarded it, and returned
    // "schema_refreshed" - so nothing was ever refreshed.
    if (!config_manager_) {
        return createErrorResult(-32603, "Configuration service unavailable");
    }
    if (!db_manager_) {
        return createErrorResult(-32603, "Database service unavailable");
    }
    SchemaHandler handler(config_manager_);
    return fromHandler("flapi_refresh_schema", handler.refreshSchema(handlerRequest()));
}

// ============================================================================
// Phase 2: Template Tools
// ============================================================================

ConfigToolResult ConfigToolAdapter::executeGetTemplate(const crow::json::wvalue& args) {
    // Delegated. "Template retrieved" was returned alongside the template's
    // FILENAME - never its content - which is the same "advertised tool does
    // not do what it says" class as the rest, and it survived the first sweep
    // because it returns real data, just not the data it promises.
    if (!config_manager_) {
        return createErrorResult(-32603, "Configuration service unavailable");
    }
    std::string error_msg;
    const std::string endpoint = extractStringParam(args, "endpoint", true, error_msg);
    if (!error_msg.empty()) {
        return createErrorResult(-32602, error_msg);
    }

    const auto resolved = config_manager_->getEndpointForPath(endpoint);
    if (!resolved) {
        return createErrorResult(-32602, "Endpoint not found: " + endpoint);
    }

    // Same, for the tools that already resolved the endpoint.
    std::map<std::string, std::string> no_params;
    const auto secrets =
        collectTemplateSecrets(config_manager_.get(), *resolved, no_params);

    TemplateHandler handler(config_manager_);
    return fromHandler("flapi_get_template",
                       handler.getEndpointTemplateBySlug(handlerRequest(),
                                                         resolved->getSlug()), &secrets);
}

ConfigToolResult ConfigToolAdapter::executeUpdateTemplate(const crow::json::wvalue& args) {
    // Delegated to the same handler PUT .../template uses. This used to
    // validate the endpoint and report success while leaving the file on disk
    // untouched, quoting the length of the content it discarded.
    if (!config_manager_) {
        return createErrorResult(-32603, "Configuration service unavailable");
    }
    std::string error_msg;
    const std::string endpoint = extractStringParam(args, "endpoint", true, error_msg);
    if (!error_msg.empty()) {
        return createErrorResult(-32602, error_msg);
    }
    // Resolved through getEndpointForPath, then asked for its own slug.
    //
    // pathToSlug(endpoint) alone made these three tools disagree with their
    // siblings: getEndpointForPath pattern-matches path parameters, so
    // flapi_get_template{"endpoint":"/orders/123"} resolves while
    // flapi_expand_template with the same argument 404s, because
    // findEndpointBySlug compares slugs exactly.
    const auto resolved = config_manager_->getEndpointForPath(endpoint);
    if (!resolved) {
        return createErrorResult(-32602, "Endpoint not found: " + endpoint);
    }
    const std::string slug = resolved->getSlug();
    std::string content_error;
    const std::string content = extractStringParam(args, "content", true, content_error);
    if (!content_error.empty()) {
        return createErrorResult(-32602, content_error);
    }

    crow::json::wvalue payload;
    payload["template"] = content;
    // Same, for the tools that already resolved the endpoint.
    std::map<std::string, std::string> no_params;
    const auto secrets =
        collectTemplateSecrets(config_manager_.get(), *resolved, no_params);

    TemplateHandler handler(config_manager_);
    return fromHandler("flapi_update_template",
                       handler.updateEndpointTemplateBySlug(handlerRequest(payload.dump()), slug), &secrets);
}

ConfigToolResult ConfigToolAdapter::executeExpandTemplate(const crow::json::wvalue& args) {
    // Delegated. This returned a hardcoded `SELECT * FROM data WHERE 1=1`
    // with "Template expanded successfully" - indistinguishable, to an agent,
    // from a real expansion of the endpoint's actual template.
    if (!config_manager_) {
        return createErrorResult(-32603, "Configuration service unavailable");
    }
    std::string error_msg;
    const std::string endpoint = extractStringParam(args, "endpoint", true, error_msg);
    if (!error_msg.empty()) {
        return createErrorResult(-32602, error_msg);
    }
    // Resolved through getEndpointForPath, then asked for its own slug.
    //
    // pathToSlug(endpoint) alone made these three tools disagree with their
    // siblings: getEndpointForPath pattern-matches path parameters, so
    // flapi_get_template{"endpoint":"/orders/123"} resolves while
    // flapi_expand_template with the same argument 404s, because
    // findEndpointBySlug compares slugs exactly.
    const auto resolved = config_manager_->getEndpointForPath(endpoint);
    if (!resolved) {
        return createErrorResult(-32602, "Endpoint not found: " + endpoint);
    }
    const std::string slug = resolved->getSlug();

    // The handler requires a `parameters` object; an absent one is an empty
    // parameter set, not an error.
    crow::json::wvalue payload;
    payload["parameters"] = crow::json::wvalue::object();
    const auto parsed = crow::json::load(crow::json::wvalue(args).dump());
    if (parsed && parsed.has("params") && parsed["params"].t() == crow::json::type::Object) {
        payload["parameters"] = crow::json::wvalue(parsed["params"]);
    } else if (parsed && parsed.has("parameters") &&
               parsed["parameters"].t() == crow::json::type::Object) {
        payload["parameters"] = crow::json::wvalue(parsed["parameters"]);
    }

    // The secret set for THIS endpoint, so a failure body quoting the
    // rendered statement is scrubbed at the boundary.
    TemplateSecrets secrets;
    if (const auto ep = config_manager_->getEndpointForPath(endpoint)) {
        std::map<std::string, std::string> rendered_params;
        secrets = collectTemplateSecrets(config_manager_.get(), *ep, rendered_params);
    }

    TemplateHandler handler(config_manager_);
    return fromHandler("flapi_expand_template",
                       handler.expandTemplateBySlug(handlerRequest(payload.dump()), slug), &secrets);
}

ConfigToolResult ConfigToolAdapter::executeTestTemplate(const crow::json::wvalue& args) {
    // Delegated. This reported "Template test passed" for a query it never
    // ran, against SQL it never produced. Of everything a stub can say,
    // claiming a test passed is the worst.
    if (!config_manager_) {
        return createErrorResult(-32603, "Configuration service unavailable");
    }
    std::string error_msg;
    const std::string endpoint = extractStringParam(args, "endpoint", true, error_msg);
    if (!error_msg.empty()) {
        return createErrorResult(-32602, error_msg);
    }
    // Resolved through getEndpointForPath, then asked for its own slug.
    //
    // pathToSlug(endpoint) alone made these three tools disagree with their
    // siblings: getEndpointForPath pattern-matches path parameters, so
    // flapi_get_template{"endpoint":"/orders/123"} resolves while
    // flapi_expand_template with the same argument 404s, because
    // findEndpointBySlug compares slugs exactly.
    const auto resolved = config_manager_->getEndpointForPath(endpoint);
    if (!resolved) {
        return createErrorResult(-32602, "Endpoint not found: " + endpoint);
    }
    const std::string slug = resolved->getSlug();

    crow::json::wvalue payload;
    payload["parameters"] = crow::json::wvalue::object();
    const auto parsed = crow::json::load(crow::json::wvalue(args).dump());
    if (parsed && parsed.has("params") && parsed["params"].t() == crow::json::type::Object) {
        payload["parameters"] = crow::json::wvalue(parsed["params"]);
    } else if (parsed && parsed.has("parameters") &&
               parsed["parameters"].t() == crow::json::type::Object) {
        payload["parameters"] = crow::json::wvalue(parsed["parameters"]);
    }

    // The secret set for THIS endpoint, so a failure body quoting the
    // rendered statement is scrubbed at the boundary.
    TemplateSecrets secrets;
    if (const auto ep = config_manager_->getEndpointForPath(endpoint)) {
        std::map<std::string, std::string> rendered_params;
        secrets = collectTemplateSecrets(config_manager_.get(), *ep, rendered_params);
    }

    TemplateHandler handler(config_manager_);
    return fromHandler("flapi_test_template",
                       handler.testTemplateBySlug(handlerRequest(payload.dump()), slug), &secrets);
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
        const auto endpoints = config_manager_->getEndpoints();   // pinned snapshot

        // Build endpoint list efficiently
        std::vector<crow::json::wvalue> endpoint_items;
        endpoint_items.reserve(endpoints->size());

        for (const auto& ep : *endpoints) {
            crow::json::wvalue endpoint_info;
            endpoint_info["name"] = ep.getName();
            endpoint_info["path"] = ep.urlPath;
            endpoint_info["method"] = ep.method;
            endpoint_info["type"] = (ep.urlPath.empty() ? "mcp" : "rest");
            endpoint_items.emplace_back(std::move(endpoint_info));
        }

        crow::json::wvalue result;
        result["count"] = static_cast<int>(endpoints->size());
        auto endpoints_list = crow::json::wvalue::list();
        for (auto& item : endpoint_items) {
            endpoints_list.emplace_back(std::move(item));
        }
        result["endpoints"] = std::move(endpoints_list);

        CROW_LOG_INFO << "flapi_list_endpoints: returned " << endpoints->size() << " endpoints";
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

        // Build connections list efficiently
        auto conn_list = crow::json::wvalue::list();
        for (const auto& conn : ep->connection) {
            conn_list.emplace_back(conn);
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
    std::string path;  // Declare outside try block for catch access
    std::string method;  // Declare outside try block for catch access
    try {
        // Defensive check: ensure config manager is available
        if (!config_manager_) {
            CROW_LOG_ERROR << "flapi_create_endpoint: ConfigManager is null";
            return createErrorResult(-32603, "Configuration service unavailable");
        }

        // Extract required parameters
        std::string error_msg = "";
        path = extractStringParam(args, "path", true, error_msg);
        if (!error_msg.empty()) {
            return createErrorResult(-32602, error_msg);
        }

        // Validate path to prevent traversal attacks
        error_msg = isValidEndpointPath(path);
        if (!error_msg.empty()) {
            return createErrorResult(-32602, error_msg);
        }

        method = extractStringParam(args, "method", false, error_msg);
        if (method.empty()) {
            method = "GET";
        }

        std::string template_source = extractStringParam(args, "template_source", false, error_msg);

        // Check if endpoint already exists
        if (config_manager_->getEndpointForPath(path) != nullptr) {
            crow::json::wvalue error_detail;
            error_detail["error"] = "Endpoint already exists";
            error_detail["path"] = path;
            error_detail["hint"] = "Use flapi_update_endpoint to modify existing endpoint, or delete it first with flapi_delete_endpoint";
            return createErrorResult(-32603, error_detail.dump());
        }

        // Create new endpoint configuration
        EndpointConfig new_endpoint;
        new_endpoint.urlPath = path;
        new_endpoint.method = method;
        new_endpoint.templateSource = template_source;

        // Add endpoint to config manager
        config_manager_->addEndpoint(new_endpoint);

        crow::json::wvalue result;
        result["status"] = "success";
        result["path"] = path;
        result["method"] = method;
        result["template_source"] = template_source;
        result["message"] = "Endpoint created successfully";

        CROW_LOG_INFO << "flapi_create_endpoint: created endpoint " << path << " with method " << method;
        return createSuccessResult(result.dump());
    } catch (const std::exception& e) {
        CROW_LOG_ERROR << "flapi_create_endpoint failed for path '" << path << "': " << e.what();
        crow::json::wvalue error_detail;
        error_detail["error"] = "Failed to create endpoint";
        error_detail["path"] = path;
        error_detail["method"] = method;
        error_detail["reason"] = std::string(e.what());
        return createErrorResult(-32603, error_detail.dump());
    }
}

ConfigToolResult ConfigToolAdapter::executeUpdateEndpoint(const crow::json::wvalue& args) {
    std::string path;  // Declare outside try block for catch access
    try {
        // Defensive check: ensure config manager is available
        if (!config_manager_) {
            CROW_LOG_ERROR << "flapi_update_endpoint: ConfigManager is null";
            return createErrorResult(-32603, "Configuration service unavailable");
        }

        // Extract endpoint path (required)
        std::string error_msg = "";
        path = extractStringParam(args, "path", true, error_msg);
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
            crow::json::wvalue error_detail;
            error_detail["error"] = "Endpoint not found";
            error_detail["path"] = path;
            error_detail["hint"] = "Use flapi_list_endpoints to see available endpoints or flapi_create_endpoint to create new one";
            return createErrorResult(-32603, error_detail.dump());
        }

        // Create updated copy
        EndpointConfig updated = *ep;
        std::string original_method = ep->method;
        std::string original_template = ep->templateSource;

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
        result["status"] = "success";
        result["path"] = path;
        result["message"] = "Endpoint updated successfully";
        result["previous_method"] = original_method;
        result["new_method"] = updated.method;
        if (original_template != updated.templateSource) {
            result["previous_template"] = original_template;
            result["new_template"] = updated.templateSource;
        }

        CROW_LOG_INFO << "flapi_update_endpoint: updated endpoint " << path << " - method: " << original_method << " -> " << updated.method;
        return createSuccessResult(result.dump());
    } catch (const std::exception& e) {
        CROW_LOG_ERROR << "flapi_update_endpoint failed for path '" << path << "': " << e.what();
        crow::json::wvalue error_detail;
        error_detail["error"] = "Failed to update endpoint";
        error_detail["path"] = path;
        error_detail["reason"] = std::string(e.what());
        error_detail["hint"] = "Ensure endpoint exists and is not in use by active requests";
        return createErrorResult(-32603, error_detail.dump());
    }
}

ConfigToolResult ConfigToolAdapter::executeDeleteEndpoint(const crow::json::wvalue& args) {
    std::string path;  // Declare outside try block for catch access
    try {
        // Defensive check: ensure config manager is available
        if (!config_manager_) {
            CROW_LOG_ERROR << "flapi_delete_endpoint: ConfigManager is null";
            return createErrorResult(-32603, "Configuration service unavailable");
        }

        // Extract endpoint path parameter
        std::string error_msg = "";
        path = extractStringParam(args, "path", true, error_msg);
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
            crow::json::wvalue error_detail;
            error_detail["error"] = "Endpoint not found";
            error_detail["path"] = path;
            error_detail["hint"] = "Use flapi_list_endpoints to see available endpoints";
            return createErrorResult(-32603, error_detail.dump());
        }

        std::string deleted_method = ep->method;
        std::string deleted_template = ep->templateSource;

        // Remove the endpoint
        bool removed = config_manager_->removeEndpointByPath(path);
        if (!removed) {
            crow::json::wvalue error_detail;
            error_detail["error"] = "Failed to delete endpoint";
            error_detail["path"] = path;
            error_detail["method"] = deleted_method;
            error_detail["hint"] = "Ensure no active requests are using this endpoint";
            return createErrorResult(-32603, error_detail.dump());
        }

        crow::json::wvalue result;
        result["status"] = "success";
        result["path"] = path;
        result["message"] = "Endpoint deleted successfully";
        result["deleted_method"] = deleted_method;
        result["deleted_template"] = deleted_template;

        CROW_LOG_INFO << "flapi_delete_endpoint: deleted endpoint " << path << " (method=" << deleted_method << ")";
        return createSuccessResult(result.dump());
    } catch (const std::exception& e) {
        CROW_LOG_ERROR << "flapi_delete_endpoint failed for path '" << path << "': " << e.what();
        crow::json::wvalue error_detail;
        error_detail["error"] = "Failed to delete endpoint";
        error_detail["path"] = path;
        error_detail["reason"] = std::string(e.what());
        return createErrorResult(-32603, error_detail.dump());
    }
}

ConfigToolResult ConfigToolAdapter::executeReloadEndpoint(const crow::json::wvalue& args) {
    std::string path;  // Declare outside try block for catch access
    try {
        // Defensive check: ensure config manager is available
        if (!config_manager_) {
            CROW_LOG_ERROR << "flapi_reload_endpoint: ConfigManager is null";
            return createErrorResult(-32603, "Configuration service unavailable");
        }

        // Extract endpoint path or slug parameter
        std::string error_msg = "";
        path = extractStringParam(args, "path", true, error_msg);
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
            crow::json::wvalue error_detail;
            error_detail["error"] = "Endpoint not found";
            error_detail["path"] = path;
            error_detail["hint"] = "Use flapi_list_endpoints to see available endpoints";
            return createErrorResult(-32603, error_detail.dump());
        }

        std::string original_method = ep->method;
        std::string original_template = ep->templateSource;

        // Reload the endpoint configuration from disk
        bool reloaded = config_manager_->reloadEndpointConfig(path);
        if (!reloaded) {
            crow::json::wvalue error_detail;
            error_detail["error"] = "Failed to reload endpoint configuration from disk";
            error_detail["path"] = path;
            error_detail["hint"] = "Verify that endpoint YAML file exists and is valid";
            return createErrorResult(-32603, error_detail.dump());
        }

        crow::json::wvalue result;
        result["status"] = "success";
        result["path"] = path;
        result["message"] = "Endpoint configuration reloaded from disk";
        result["original_method"] = original_method;
        result["original_template"] = original_template;

        CROW_LOG_INFO << "flapi_reload_endpoint: reloaded endpoint " << path << " from disk";
        return createSuccessResult(result.dump());
    } catch (const std::exception& e) {
        CROW_LOG_ERROR << "flapi_reload_endpoint failed for path '" << path << "': " << e.what();
        crow::json::wvalue error_detail;
        error_detail["error"] = "Failed to reload endpoint";
        error_detail["path"] = path;
        error_detail["reason"] = std::string(e.what());
        error_detail["hint"] = "Check server logs for more details";
        return createErrorResult(-32603, error_detail.dump());
    }
}

// ============================================================================
// ============================================================================
// Phase 4: Cache Tools
// ============================================================================

ConfigToolResult ConfigToolAdapter::executeGetCacheStatus(const crow::json::wvalue& args) {
    // Delegated to the same handler GET .../cache uses.
    //
    // The hand-rolled version echoed three fields of static YAML back and
    // labelled it "success", while its description promised "snapshot history
    // and refresh timestamps". An agent asking when a cache last refreshed
    // got a successful answer with the question silently dropped, and could
    // not tell that apart from a cache that had never refreshed.
    if (!config_manager_) {
        return createErrorResult(-32603, "Configuration service unavailable");
    }
    std::string error_msg;
    const std::string endpoint_path = extractStringParam(args, "path", true, error_msg);
    if (!error_msg.empty()) {
        return createErrorResult(-32602, error_msg);
    }

    // The secret set for THIS endpoint, so a failure body quoting the
    // rendered statement is scrubbed at the boundary rather than made opaque.
    TemplateSecrets secrets;
    if (const auto ep = config_manager_->getEndpointForPath(endpoint_path)) {
        std::map<std::string, std::string> no_params;
        secrets = collectTemplateSecrets(config_manager_.get(), *ep, no_params);
    }

    CacheConfigHandler handler(config_manager_);
    return fromHandler("flapi_get_cache_status",
                       handler.getCacheConfig(handlerRequest(), endpoint_path), &secrets);
}

ConfigToolResult ConfigToolAdapter::executeRefreshCache(const crow::json::wvalue& args) {
    // Delegated to the same handler POST .../cache/refresh uses. This adapter
    // held no CacheManager reference at all, and returned "Cache refresh has
    // been scheduled" - a mutation claiming work nothing had queued.
    if (!config_manager_) {
        return createErrorResult(-32603, "Configuration service unavailable");
    }
    std::string error_msg;
    const std::string endpoint_path = extractStringParam(args, "path", true, error_msg);
    if (!error_msg.empty()) {
        return createErrorResult(-32602, error_msg);
    }
    // The secret set for THIS endpoint, so a failure body quoting the
    // rendered statement is scrubbed at the boundary rather than made opaque.
    TemplateSecrets secrets;
    if (const auto ep = config_manager_->getEndpointForPath(endpoint_path)) {
        std::map<std::string, std::string> no_params;
        secrets = collectTemplateSecrets(config_manager_.get(), *ep, no_params);
    }

    CacheConfigHandler handler(config_manager_);
    return fromHandler("flapi_refresh_cache",
                       handler.refreshCache(handlerRequest(), endpoint_path), &secrets);
}

ConfigToolResult ConfigToolAdapter::executeGetCacheAudit(const crow::json::wvalue& args) {
    // Delegated. This INVENTED an audit entry from std::time(nullptr) under
    // the comment "Add sample audit entry" and returned it as a retrieved
    // audit log - manufactured records, handed to an agent asking what had
    // actually happened.
    if (!config_manager_) {
        return createErrorResult(-32603, "Configuration service unavailable");
    }
    std::string error_msg;
    const std::string endpoint_path = extractStringParam(args, "path", true, error_msg);
    if (!error_msg.empty()) {
        return createErrorResult(-32602, error_msg);
    }
    // The secret set for THIS endpoint, so a failure body quoting the
    // rendered statement is scrubbed at the boundary rather than made opaque.
    TemplateSecrets secrets;
    if (const auto ep = config_manager_->getEndpointForPath(endpoint_path)) {
        std::map<std::string, std::string> no_params;
        secrets = collectTemplateSecrets(config_manager_.get(), *ep, no_params);
    }

    AuditLogHandler handler(config_manager_);
    return fromHandler("flapi_get_cache_audit",
                       handler.getCacheAuditLog(endpoint_path), &secrets);
}

ConfigToolResult ConfigToolAdapter::executeRunCacheGC(const crow::json::wvalue& args) {
    // Delegated. This returned "Garbage collection triggered" having
    // triggered nothing.
    if (!config_manager_) {
        return createErrorResult(-32603, "Configuration service unavailable");
    }
    // `path` is REQUIRED. The comment here used to say an absent path meant
    // "every cached endpoint", but the handler's first act is
    // getEndpointForPath(""), which matches nothing - so the documented
    // no-argument form returned "Endpoint not found" and collected nothing.
    // There is no all-caches GC route to delegate to; the REST route is
    // POST /api/v1/_config/endpoints/{slug}/cache/gc, one endpoint at a time.
    std::string error_msg;
    const std::string endpoint_path = extractStringParam(args, "path", true, error_msg);
    if (!error_msg.empty()) {
        return createErrorResult(-32602, error_msg);
    }

    // The secret set for THIS endpoint, so a failure body quoting the
    // rendered statement is scrubbed at the boundary rather than made opaque.
    TemplateSecrets secrets;
    if (const auto ep = config_manager_->getEndpointForPath(endpoint_path)) {
        std::map<std::string, std::string> no_params;
        secrets = collectTemplateSecrets(config_manager_.get(), *ep, no_params);
    }

    CacheConfigHandler handler(config_manager_);
    return fromHandler("flapi_run_cache_gc",
                       handler.performGarbageCollection(handlerRequest(), endpoint_path), &secrets);
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
    schema["properties"] = crow::json::wvalue::object();
    return schema;
}

crow::json::wvalue ConfigToolAdapter::buildOutputSchema() {
    crow::json::wvalue schema;
    schema["type"] = "object";
    schema["properties"] = crow::json::wvalue::object();
    return schema;
}

std::string ConfigToolAdapter::isValidEndpointPath(const std::string& path) {
    // Check for empty path
    if (path.empty()) {
        return "Endpoint path cannot be empty";
    }

    // URL paths naturally start with '/' - this is valid for endpoint paths
    // We only check for path traversal attacks

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

bool ConfigToolAdapter::tokenMatchesConfigured(const std::string& auth_token) const {
    // Fail closed: a tool marked auth_required with no configured secret has
    // nothing to authenticate against, and waving it through is how the
    // original defect behaved.
    if (expected_auth_token_.empty()) {
        return false;
    }

    // Accept both "<scheme> <token>" and a bare token, matching what
    // validateAuthToken permits.
    std::string presented = auth_token;
    const size_t space_pos = auth_token.find(' ');
    if (space_pos != std::string::npos) {
        presented = auth_token.substr(space_pos + 1);
    }

    // Constant time, so the comparison cannot be turned into an oracle that
    // reveals the token a character at a time. Length is compared first and
    // that does leak the length; CRYPTO_memcmp needs equal sizes.
    if (presented.size() != expected_auth_token_.size()) {
        return false;
    }
    return CRYPTO_memcmp(presented.data(), expected_auth_token_.data(),
                         presented.size()) == 0;
}

std::string ConfigToolAdapter::validateAuthToken(const std::string& auth_token) {
    // Check for empty token
    if (auth_token.empty()) {
        return "Authentication token is required";
    }

    // Minimum token length check
    if (auth_token.length() < 8) {
        return "Authentication token is too short (minimum 8 characters)";
    }

    // Maximum token length check (prevent DoS via huge tokens)
    if (auth_token.length() > 4096) {
        return "Authentication token is too long (maximum 4096 characters)";
    }

    // Parse token format
    size_t space_pos = auth_token.find(' ');

    if (space_pos == std::string::npos) {
        // No space found - could be a raw token, check if it looks like a valid token
        // Should contain only alphanumeric, dash, underscore, dot, or equals sign (for Base64)
        for (char c : auth_token) {
            if (!std::isalnum(c) && c != '-' && c != '_' && c != '.' && c != '=' && c != ':') {
                return "Authentication token contains invalid characters";
            }
        }
        // Raw token format is valid (API tokens, simple tokens)
        CROW_LOG_DEBUG << "Token validation: raw token format accepted";
        return "";
    }

    // Token has a scheme prefix (Bearer, Basic, Token, etc.)
    std::string scheme = auth_token.substr(0, space_pos);
    std::string token_value = auth_token.substr(space_pos + 1);

    // Validate scheme
    if (scheme != "Bearer" && scheme != "Basic" && scheme != "Token" && scheme != "API-Key") {
        return "Unsupported authentication scheme: " + scheme +
               " (supported: Bearer, Basic, Token, API-Key)";
    }

    // Validate token value is not empty
    if (token_value.empty()) {
        return scheme + " token cannot be empty";
    }

    // Bearer tokens should contain Base64 characters
    if (scheme == "Bearer" || scheme == "Basic") {
        for (char c : token_value) {
            if (!std::isalnum(c) && c != '+' && c != '/' && c != '-' && c != '_' && c != '=') {
                return scheme + " token contains invalid Base64 characters";
            }
        }
    }

    // Validate token value length (minimum and maximum)
    if (token_value.length() < 4) {
        return scheme + " token is too short (minimum 4 characters)";
    }

    if (token_value.length() > 2048) {
        return scheme + " token is too long (maximum 2048 characters)";
    }

    // For Bearer tokens, ensure the Base64 length is valid
    if (scheme == "Bearer" && token_value.length() % 4 != 0 && token_value.find('=') == std::string::npos) {
        // Note: Base64 can have padding variations, so we're lenient here
        CROW_LOG_DEBUG << "Bearer token Base64 length may need padding validation";
    }

    CROW_LOG_INFO << "Token validation successful: scheme=" << scheme << ", token_length=" << token_value.length();
    return "";  // Token is valid
}

}  // namespace flapi
