#include "open_api_doc_generator.hpp"
#include "mcp_schema_builder.hpp"
#include <sstream>
#include <map>
#include <vector>

namespace flapi {

OpenAPIDocGenerator::OpenAPIDocGenerator(std::shared_ptr<ConfigManager> cm, std::shared_ptr<DatabaseManager> dm)
    : configManager(cm), dbManager(dm) {}

YAML::Node OpenAPIDocGenerator::generateDoc(flapi::FlapiApp& app) 
{
    YAML::Node doc;
    
    // OpenAPI version
    doc["openapi"] = "3.0.0";
    
    // Info Object
    doc["info"]["title"] = configManager->getProjectName();
    doc["info"]["version"] = "1.0.0";
    doc["info"]["description"] = configManager->getProjectDescription();

    // Servers
    doc["servers"].push_back(YAML::Node());
    std::string serverUrl = app.ssl_used() ? "https://" : "http://";
    serverUrl += "localhost";
    serverUrl += ":" + std::to_string(app.port());
    doc["servers"][0]["url"] = serverUrl;

    // Security Schemes - define both types
    doc["components"]["securitySchemes"]["bearerAuth"]["type"] = "http";
    doc["components"]["securitySchemes"]["bearerAuth"]["scheme"] = "bearer";
    doc["components"]["securitySchemes"]["bearerAuth"]["bearerFormat"] = "JWT";
    doc["components"]["securitySchemes"]["bearerAuth"]["description"] = "JWT Authorization header using the Bearer scheme.";
    
    doc["components"]["securitySchemes"]["basicAuth"]["type"] = "http";
    doc["components"]["securitySchemes"]["basicAuth"]["scheme"] = "basic";
    doc["components"]["securitySchemes"]["basicAuth"]["description"] = "Basic HTTP Authentication";

    // Paths
    // Group endpoints by URL path to merge multiple HTTP methods for the same path
    std::map<std::string, std::vector<EndpointConfig>> endpointsByPath;
    for (const auto& endpoint : *configManager->getEndpoints()) {
        if (!endpoint.isRESTEndpoint()) {
            continue;   // an MCP-only tool/resource/prompt has no URL to document
        }
        endpointsByPath[endpoint.urlPath].push_back(endpoint);
    }

    // Generate path items, merging multiple methods for the same path
    for (const auto& [path, endpoints] : endpointsByPath) {
        YAML::Node pathItem;
        for (const auto& endpoint : endpoints) {
            std::string method = endpoint.method.empty() ? "get" : endpoint.method;
            std::transform(method.begin(), method.end(), method.begin(), ::tolower);
            pathItem[method] = generateOperation(endpoint, method);
        }

        // Add DELETE endpoint documentation for cache invalidation if any endpoint has cache enabled
        bool hasCacheEnabled = false;
        for (const auto& endpoint : endpoints) {
            if (dbManager->isCacheEnabled(endpoint)) {
                hasCacheEnabled = true;
                break;
            }
        }

        if (hasCacheEnabled && !pathItem["delete"]) {
            YAML::Node deleteOperation;
            deleteOperation["summary"] = "Invalidate cache for " + path;
            deleteOperation["description"] = "Invalidates the cached data for this endpoint";

            deleteOperation["responses"]["200"]["description"] = "Cache successfully invalidated";
            deleteOperation["responses"]["500"]["description"] = "Internal server error while invalidating cache";

            // Add security if any endpoint requires auth
            bool requiresAuth = false;
            std::string authType = "basic";
            for (const auto& endpoint : endpoints) {
                if (endpoint.auth.enabled) {
                    requiresAuth = true;
                    authType = endpoint.auth.type;
                    break;
                }
            }

            if (requiresAuth) {
                addSecurity(deleteOperation, authType);
            }

            pathItem["delete"] = deleteOperation;
        }

        doc["paths"][path] = pathItem;
    }

    return doc;
}

namespace {

// A crow JSON value as a YAML node. JSON is YAML, so the dump parses as-is.
YAML::Node toYaml(const crow::json::wvalue& value) {
    return YAML::Load(value.dump());
}

} // namespace

void OpenAPIDocGenerator::addSecurity(YAML::Node& operation, const std::string& auth_type) {
    YAML::Node requirement;
    requirement[auth_type == "basic" ? "basicAuth" : "bearerAuth"] = YAML::Node(YAML::NodeType::Sequence);
    operation["security"].push_back(requirement);
}

YAML::Node OpenAPIDocGenerator::generateOperation(const EndpointConfig& endpoint, const std::string& method) {
    YAML::Node operation;
    operation["summary"] = "Endpoint for " + endpoint.urlPath;
    // The endpoint's own `description:`, else its MCP tool's. Neither: no key,
    // rather than the old "Description not available" on every operation.
    if (!endpoint.description.empty()) {
        operation["description"] = endpoint.description;
    } else if (endpoint.mcp_tool && !endpoint.mcp_tool->description.empty()) {
        operation["description"] = endpoint.mcp_tool->description;
    }

    const bool is_read = endpoint.operation.type != OperationConfig::Write;
    YAML::Node parameters = generateParameters(endpoint.request_fields);
    if (is_read) {
        addReadParameters(parameters, endpoint);
    }
    if (parameters.size() > 0) {
        operation["parameters"] = parameters;
    }

        operation["responses"]["200"]["description"] = "Successful response";
    if (is_read) {
        operation["responses"]["200"]["content"]["application/json"]["schema"] = generateResponseSchema(endpoint);
        operation["responses"]["200"]["content"]["text/csv"]["schema"]["type"] = "string";
        // What every read answers with, whatever its format (request_handler.cpp).
        const std::vector<std::pair<std::string, std::string>> headers = {
            {"X-Total-Count", "Total number of rows the query matches, before limit/offset"},
            {"X-Offset", "The offset this page starts at"},
            {"X-Limit", "The page size applied"},
            {"X-Next", "Path and query of the next page; empty on the last page"},
        };
        for (const auto& [name, description] : headers) {
            YAML::Node header;
            header["description"] = description;
            header["schema"]["type"] = (name == "X-Next") ? "string" : "integer";
            operation["responses"]["200"]["headers"][name] = header;
        }
    } else {
        YAML::Node schema;
        schema["type"] = "object";
        schema["properties"]["rows_affected"]["type"] = "integer";
        schema["properties"]["data"]["type"] = "array";
        schema["properties"]["data"]["items"]["type"] = "object";
        operation["responses"]["200"]["content"]["application/json"]["schema"] = schema;
    }

    // Every endpoint validates its input, and says why it refused.
    {
        YAML::Node bad;
        bad["description"] = "Invalid parameters";
        YAML::Node schema;
        schema["type"] = "object";
        schema["properties"]["errors"]["type"] = "array";
        schema["properties"]["errors"]["items"]["type"] = "object";
        schema["properties"]["errors"]["items"]["properties"]["field"]["type"] = "string";
        schema["properties"]["errors"]["items"]["properties"]["message"]["type"] = "string";
        schema["properties"]["error"]["type"] = "string";
        bad["content"]["application/json"]["schema"] = schema;
        operation["responses"]["400"] = bad;
    }

    if (endpoint.rate_limit.enabled) {
        operation["x-rate-limit"]["max"] = endpoint.rate_limit.max;
        operation["x-rate-limit"]["interval"] = endpoint.rate_limit.interval;
    }

    if (endpoint.auth.enabled) {
        addSecurity(operation, endpoint.auth.type);
    }

    // Document the JSON body of every method that has one: a read over POST
    // (`operation.type: read`) takes its parameters from the body as well.
    if (method == "post" || method == "put" || method == "patch" || method == "delete") {
        YAML::Node schema;
        schema["type"] = "object";
        YAML::Node properties(YAML::NodeType::Map);
        for (const auto& field : endpoint.request_fields) {
            if (field.fieldIn == "body") {
                properties[field.fieldName] = toYaml(MCPSchemaBuilder::buildPropertySchema(field));
                if (field.required) {
                    schema["required"].push_back(field.fieldName);
                }
            }
        }
        schema["properties"] = properties;
        if (properties.size() > 0 || method != "delete") {
            YAML::Node requestBody;
            requestBody["description"] = "Request body with parameters for " + method + " operation";
            requestBody["required"] = method != "delete";
            requestBody["content"]["application/json"]["schema"] = schema;
            operation["requestBody"] = requestBody;
        }

        if (is_read) {
            // A read answers 200 with the read envelope, whatever its method.
        } else if (method == "post") {
            YAML::Node createdResponse = operation["responses"]["200"];
            createdResponse["description"] = "Created successfully";
            operation["responses"]["201"] = createdResponse;
            operation["responses"].remove("200");
        } else if (method == "put" || method == "patch") {
            operation["responses"]["200"]["description"] = "Updated successfully";
        }
    }

    return operation;
}

YAML::Node OpenAPIDocGenerator::generateParameters(const std::vector<RequestFieldConfig>& requestFields)
{
    YAML::Node parameters(YAML::NodeType::Sequence);

    for (const auto& field : requestFields) {
        // OpenAPI 3 has no `in: body` parameter: body fields are documented in
        // the requestBody schema instead.
        if (field.fieldIn != "query" && field.fieldIn != "path" && field.fieldIn != "header" &&
            field.fieldIn != "cookie") {
            continue;
        }
        YAML::Node parameter;
        parameter["name"] = field.fieldName;
        parameter["in"] = field.fieldIn;
        // A path parameter is always required in OpenAPI.
        parameter["required"] = field.required || field.fieldIn == "path";
        if (!field.description.empty()) {
            parameter["description"] = field.description;
        }

        // The same type, format, bounds, enum, pattern and typed default that
        // tools/list advertises; the description lives on the parameter.
        YAML::Node schema = toYaml(MCPSchemaBuilder::buildPropertySchema(field));
        schema.remove("description");
        parameter["schema"] = schema;
        parameters.push_back(parameter);
    }

    return parameters;
}

void OpenAPIDocGenerator::addReadParameters(YAML::Node& parameters, const EndpointConfig& endpoint) {
    auto declared = [&](const std::string& name) {
        for (const auto& field : endpoint.request_fields) {
            if (field.fieldName == name) {
                return true;
            }
        }
        return false;
    };
    auto add = [&](const std::string& name, const std::string& description, YAML::Node schema) {
        if (declared(name)) {
            return;   // the endpoint documents its own
        }
        YAML::Node parameter;
        parameter["name"] = name;
        parameter["in"] = "query";
        parameter["required"] = false;
        parameter["description"] = description;
        parameter["schema"] = schema;
        parameters.push_back(parameter);
    };

    if (endpoint.with_pagination) {
        YAML::Node limit;
        limit["type"] = "integer";
        limit["minimum"] = 0;
        limit["default"] = 100;
        add("limit", "Maximum number of rows to return (0 returns only the count headers)", limit);
        YAML::Node offset;
        offset["type"] = "integer";
        offset["minimum"] = 0;
        offset["default"] = 0;
        add("offset", "Number of rows to skip", offset);
    }
    YAML::Node format;
    format["type"] = "string";
    format["enum"].push_back("json");
    format["enum"].push_back("csv");
    format["enum"].push_back("arrow");
    format["default"] = "json";
    add("format", "Response format; overrides the Accept header", format);
}

YAML::Node OpenAPIDocGenerator::generateResponseSchema(const EndpointConfig& endpoint)
{
    YAML::Node schema;

    YAML::Node properties = dbManager->describeSelectQuery(endpoint);

    YAML::Node items;
    items["type"] = "object";
    // Only when the template could be described: a null here was emitted as
    // `properties: ~`, which is not a valid schema.
    if (properties.IsMap() && properties.size() > 0) {
        items["properties"] = properties;
    }

    if (endpoint.with_pagination) {
        schema["type"] = "object";
        schema["properties"]["data"]["type"] = "array";
        schema["properties"]["data"]["items"] = items;

        schema["properties"]["next"]["type"] = "string";
        if (endpoint.total_count) {
            schema["properties"]["total_count"]["type"] = "integer";
        }
    } else {
        schema["type"] = "array";
        schema["items"] = items;
    }

    return schema;
}

YAML::Node OpenAPIDocGenerator::generateConfigServiceDoc(flapi::FlapiApp& app) {
    YAML::Node doc;
    
    // OpenAPI version
    doc["openapi"] = "3.0.0";
    
    // Info Object
    doc["info"]["title"] = "Flapi Configuration Service API";
    doc["info"]["version"] = "1.0.0";
    doc["info"]["description"] = "REST API for managing Flapi endpoint configurations, filesystem operations, and system diagnostics. "
                                 "This API allows you to inspect, validate, reload, and manage endpoint configurations at runtime.";
    
    // Servers
    doc["servers"].push_back(YAML::Node());
    std::string serverUrl = app.ssl_used() ? "https://" : "http://";
    serverUrl += "localhost";
    serverUrl += ":" + std::to_string(app.port());
    doc["servers"][0]["url"] = serverUrl;
    
    // Tags
    doc["tags"].push_back(YAML::Node());
    doc["tags"][0]["name"] = "Filesystem";
    doc["tags"][0]["description"] = "Operations for browsing the template filesystem and endpoint configurations";
    
    doc["tags"].push_back(YAML::Node());
    doc["tags"][1]["name"] = "Endpoint Configuration";
    doc["tags"][1]["description"] = "Operations for managing individual endpoint configurations";
    
    doc["tags"].push_back(YAML::Node());
    doc["tags"][2]["name"] = "Audit";
    doc["tags"][2]["description"] = "Operations for accessing audit logs and system diagnostics";
    
    // Add all config service paths
    YAML::Node paths = doc["paths"];
    addConfigServicePaths(paths);
    doc["paths"] = paths;
    
    return doc;
}

void OpenAPIDocGenerator::addConfigServicePaths(YAML::Node& paths) {
    // ========================================================================
    // Filesystem Endpoints
    // ========================================================================
    
    // GET /api/v1/_config/filesystem
    {
        YAML::Node operation = createOperation(
            "Get Filesystem Structure",
            "Returns the complete directory structure of the templates folder, including all endpoint YAML files. "
            "Each file node includes metadata such as type (REST, MCP Tool, MCP Resource, MCP Prompt), URL path, "
            "authentication requirements, and caching configuration. This is used by the Flapi Explorer UI.",
            "Filesystem"
        );
        
        YAML::Node responses = operation["responses"];
        addResponse(responses, "200", 
            "Successful response with filesystem tree structure. Returns a hierarchical JSON representation of all "
            "template files and directories.");
        addResponse(responses, "500", "Internal server error");
        operation["responses"] = responses;
        
        paths["/api/v1/_config/filesystem"]["get"] = operation;
    }
    
    // ========================================================================
    // Endpoint Configuration Endpoints
    // ========================================================================
    
    // GET /api/v1/_config/endpoints/{slug}
    {
        YAML::Node operation = createOperation(
            "Get Endpoint Configuration",
            "Retrieves the complete parsed configuration for a specific endpoint by its slug (URL path or MCP name). "
            "Returns the full EndpointConfig object including request fields, authentication, caching, rate limiting, "
            "and all resolved template sources with {{include}} directives expanded.",
            "Endpoint Configuration"
        );
        
        YAML::Node params = operation["parameters"];
        addParameter(params, "slug", "path", "string", true,
            "The endpoint slug (URL path like 'customers' or full path like '/customers/'). "
            "For MCP endpoints, use the tool/resource/prompt name.");
        operation["parameters"] = params;
        
        YAML::Node responses = operation["responses"];
        addResponse(responses, "200",
            "Successful response with complete endpoint configuration");
        addResponse(responses, "404",
            "Endpoint not found - the specified slug does not match any configured endpoint");
        addResponse(responses, "500", "Internal server error");
        operation["responses"] = responses;
        
        paths["/api/v1/_config/endpoints/{slug}"]["get"] = operation;
    }
    
    // POST /api/v1/_config/endpoints/{slug}/validate
    {
        YAML::Node operation = createOperation(
            "Validate Endpoint Configuration",
            "Validates an endpoint configuration YAML file without reloading it into the running server. "
            "Performs comprehensive validation including: required fields check, SQL template syntax validation, "
            "database connection verification, file existence checks (templates, cache files), and format validation. "
            "Returns detailed validation results with errors and warnings. This is used by the VSCode extension for "
            "real-time validation feedback.",
            "Endpoint Configuration"
        );
        
        YAML::Node params = operation["parameters"];
        addParameter(params, "slug", "path", "string", true,
            "The endpoint slug to validate");
        operation["parameters"] = params;
        
        YAML::Node responses = operation["responses"];
        addResponse(responses, "200",
            "Validation completed (check 'valid' field in response). Returns ValidationResult with 'valid' boolean, "
            "'errors' array (critical issues), and 'warnings' array (non-critical issues).");
        addResponse(responses, "404",
            "Endpoint configuration file not found");
        addResponse(responses, "500",
            "Internal server error during validation");
        operation["responses"] = responses;
        
        paths["/api/v1/_config/endpoints/{slug}/validate"]["post"] = operation;
    }
    
    // POST /api/v1/_config/endpoints/{slug}/reload
    {
        YAML::Node operation = createOperation(
            "Reload Endpoint Configuration",
            "Reloads an endpoint configuration from disk into the running server without restarting. "
            "This operation: (1) validates the YAML configuration, (2) parses and resolves all {{include}} directives, "
            "(3) updates the in-memory configuration, (4) re-registers the endpoint routes. "
            "If validation fails, the existing configuration remains unchanged. "
            "Use this after editing endpoint YAML files to apply changes immediately.",
            "Endpoint Configuration"
        );
        
        YAML::Node params = operation["parameters"];
        addParameter(params, "slug", "path", "string", true,
            "The endpoint slug to reload");
        operation["parameters"] = params;
        
        YAML::Node responses = operation["responses"];
        addResponse(responses, "200",
            "Endpoint successfully reloaded. Returns ReloadResult with 'success' boolean and 'message' string.");
        addResponse(responses, "400",
            "Validation failed - configuration contains errors (existing config preserved)");
        addResponse(responses, "404",
            "Endpoint configuration file not found");
        addResponse(responses, "500",
            "Internal server error during reload");
        operation["responses"] = responses;
        
        paths["/api/v1/_config/endpoints/{slug}/reload"]["post"] = operation;
    }
    
    // POST /api/v1/_config/endpoints/{slug}
    {
        YAML::Node operation = createOperation(
            "Update Endpoint Configuration",
            "Updates an existing endpoint configuration by writing new YAML content to disk and then reloading it. "
            "The request body should contain the complete YAML configuration as a string. "
            "This operation: (1) validates the new configuration, (2) writes to the endpoint's YAML file, "
            "(3) reloads the configuration into memory, (4) re-registers routes. "
            "Used by configuration management UIs for saving endpoint changes.",
            "Endpoint Configuration"
        );
        
        YAML::Node params = operation["parameters"];
        addParameter(params, "slug", "path", "string", true,
            "The endpoint slug to update");
        operation["parameters"] = params;
        
        addRequestBody(operation, 
            "The complete YAML configuration content as a plain text string. Must be valid YAML and pass all "
            "validation checks. Extended YAML syntax ({{include}}, {{env.VAR}}) is supported.");
        
        YAML::Node responses = operation["responses"];
        addResponse(responses, "200",
            "Configuration successfully updated and reloaded");
        addResponse(responses, "400",
            "Invalid YAML or validation failed (existing config preserved)");
        addResponse(responses, "404",
            "Endpoint configuration file not found");
        addResponse(responses, "500",
            "Internal server error during update");
        operation["responses"] = responses;
        
        paths["/api/v1/_config/endpoints/{slug}"]["post"] = operation;
    }
    
    // DELETE /api/v1/_config/endpoints/{slug}
    {
        YAML::Node operation = createOperation(
            "Delete Endpoint Configuration",
            "Permanently deletes an endpoint configuration file from disk and removes the endpoint from the running server. "
            "This operation: (1) removes the YAML configuration file, (2) unregisters the endpoint routes, "
            "(3) removes the endpoint from the in-memory configuration. "
            "WARNING: This operation cannot be undone. The configuration file will be permanently deleted.",
            "Endpoint Configuration"
        );
        
        YAML::Node params = operation["parameters"];
        addParameter(params, "slug", "path", "string", true,
            "The endpoint slug to delete");
        operation["parameters"] = params;
        
        YAML::Node responses = operation["responses"];
        addResponse(responses, "200",
            "Endpoint configuration successfully deleted");
        addResponse(responses, "404",
            "Endpoint configuration not found");
        addResponse(responses, "500",
            "Internal server error during deletion");
        operation["responses"] = responses;
        
        paths["/api/v1/_config/endpoints/{slug}"]["delete"] = operation;
    }
    
    // ========================================================================
    // Audit and Diagnostics Endpoints
    // ========================================================================
    
    // GET /api/v1/_config/audit
    {
        YAML::Node operation = createOperation(
            "Get Audit Logs",
            "Retrieves system audit logs with optional filtering and pagination. "
            "Logs include: endpoint access events, configuration changes, authentication attempts, "
            "cache operations, and error events. Each log entry includes timestamp, event type, "
            "endpoint, user information, and event-specific details.",
            "Audit"
        );
        
        // Add parameters
        YAML::Node params = operation["parameters"];
        addParameter(params, "limit", "query", "integer", false,
            "Maximum number of log entries to return (default: 100, max: 1000)");
        addParameter(params, "offset", "query", "integer", false,
            "Number of log entries to skip for pagination (default: 0)");
        addParameter(params, "level", "query", "string", false,
            "Filter by log level: DEBUG, INFO, WARNING, ERROR");
        addParameter(params, "endpoint", "query", "string", false,
            "Filter logs by specific endpoint slug");
        addParameter(params, "start_time", "query", "string", false,
            "Filter logs after this timestamp (ISO 8601 format: 2024-01-01T00:00:00Z)");
        addParameter(params, "end_time", "query", "string", false,
            "Filter logs before this timestamp (ISO 8601 format)");
        operation["parameters"] = params;
        
        // Add responses
        YAML::Node responses = operation["responses"];
        addResponse(responses, "200",
            "Successful response with array of audit log entries");
        addResponse(responses, "400",
            "Invalid query parameters (e.g., invalid date format)");
        addResponse(responses, "500",
            "Internal server error");
        operation["responses"] = responses;
        
        paths["/api/v1/_config/audit"]["get"] = operation;
    }
    
    // GET /api/v1/_config/health
    {
        YAML::Node operation = createOperation(
            "Health Check",
            "Returns the health status of the Flapi server and all subsystems. "
            "Includes: server uptime, database connection status, cache manager status, "
            "number of configured endpoints, memory usage, and recent error counts. "
            "Use this for monitoring and alerting.",
            "Audit"
        );
        
        YAML::Node responses = operation["responses"];
        addResponse(responses, "200",
            "Server is healthy - all subsystems operational");
        addResponse(responses, "503",
            "Server is unhealthy - one or more subsystems degraded or unavailable");
        operation["responses"] = responses;
        
        paths["/api/v1/_config/health"]["get"] = operation;
    }
    
    // GET /api/v1/_config/metrics
    {
        YAML::Node operation = createOperation(
            "Get System Metrics",
            "Returns runtime counters: tracing span export (enabled, spans_exported, "
            "spans_dropped), Arrow streaming (total/successful/failed requests, "
            "total_rows, active_streams), and the loaded endpoint count. Counters are "
            "cumulative since process start. Requires the config-service token.",
            "Audit"
        );

        YAML::Node responses = operation["responses"];
        addResponse(responses, "200",
            "Runtime counters for tracing, Arrow streaming and endpoint count");
        addResponse(responses, "401",
            "Missing or invalid config-service token");
        operation["responses"] = responses;
        
        paths["/api/v1/_config/metrics"]["get"] = operation;
    }
}

YAML::Node OpenAPIDocGenerator::createOperation(const std::string& summary, 
                                                const std::string& description,
                                                const std::string& tag) {
    YAML::Node operation;
    operation["summary"] = summary;
    operation["description"] = description;
    
    if (!tag.empty()) {
        operation["tags"].push_back(tag);
    }
    
    return operation;
}

void OpenAPIDocGenerator::addParameter(YAML::Node& parameters, 
                                       const std::string& name, 
                                       const std::string& in,
                                       const std::string& type, 
                                       bool required, 
                                       const std::string& description) {
    YAML::Node param;
    param["name"] = name;
    param["in"] = in;
    param["required"] = required;
    param["description"] = description;
    param["schema"]["type"] = type;
    
    // Convert to sequence if not already
    if (!parameters.IsDefined() || parameters.IsNull()) {
        parameters = YAML::Node(YAML::NodeType::Sequence);
    }
    parameters.push_back(param);
}

void OpenAPIDocGenerator::addResponse(YAML::Node& responses, 
                                      const std::string& code,
                                      const std::string& description,
                                      const std::string& schema_ref) {
    // Create the response entry
    if (!responses.IsDefined() || responses.IsNull()) {
        responses = YAML::Node(YAML::NodeType::Map);
    }
    responses[code]["description"] = description;
    
    if (!schema_ref.empty()) {
        responses[code]["content"]["application/json"]["schema"]["$ref"] = schema_ref;
    } else {
        responses[code]["content"]["application/json"]["schema"]["type"] = "object";
    }
}

void OpenAPIDocGenerator::addRequestBody(YAML::Node& operation, 
                                        const std::string& description,
                                        const std::string& schema_ref) {
    operation["requestBody"]["description"] = description;
    operation["requestBody"]["required"] = true;
    
    if (!schema_ref.empty()) {
        operation["requestBody"]["content"]["text/plain"]["schema"]["$ref"] = schema_ref;
    } else {
        operation["requestBody"]["content"]["text/plain"]["schema"]["type"] = "string";
    }
}

} // namespace flapi