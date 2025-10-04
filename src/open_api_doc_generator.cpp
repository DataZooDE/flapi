#include "open_api_doc_generator.hpp"
#include <sstream>

namespace flapi {

OpenAPIDocGenerator::OpenAPIDocGenerator(std::shared_ptr<ConfigManager> cm, std::shared_ptr<DatabaseManager> dm)
    : configManager(cm), dbManager(dm) {}

YAML::Node OpenAPIDocGenerator::generateDoc(crow::App<crow::CORSHandler, RateLimitMiddleware, AuthMiddleware>& app) 
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
    for (const auto& endpoint : configManager->getEndpoints()) {
        doc["paths"][endpoint.urlPath] = generatePathItem(endpoint);
    }

    return doc;
}

YAML::Node OpenAPIDocGenerator::generatePathItem(const EndpointConfig& endpoint)
{
    YAML::Node pathItem;
    
    // Generate GET endpoint documentation
    std::string method = endpoint.method.empty() ? "get" : endpoint.method;
    std::transform(method.begin(), method.end(), method.begin(), ::tolower);
    
    YAML::Node operation;
    operation["summary"] = "Endpoint for " + endpoint.urlPath;
    operation["description"] = "Description not available";
    operation["parameters"] = generateParameters(endpoint.request_fields);
    operation["responses"]["200"]["description"] = "Successful response";
    operation["responses"]["200"]["content"]["application/json"]["schema"] = generateResponseSchema(endpoint);
    
    if (endpoint.rate_limit.enabled) {
        operation["x-rate-limit"]["max"] = endpoint.rate_limit.max;
        operation["x-rate-limit"]["interval"] = endpoint.rate_limit.interval;
    }
    
    // Add security requirement based on auth type
    if (endpoint.auth.enabled) {
        operation["security"].push_back(YAML::Node());
        if (endpoint.auth.type == "basic") {
            operation["security"][0]["basicAuth"] = YAML::Node(YAML::NodeType::Sequence);
        } else {
            operation["security"][0]["bearerAuth"] = YAML::Node(YAML::NodeType::Sequence);
        }
    }
    
    pathItem[method] = operation;

    // Add DELETE endpoint documentation if cache is enabled
    if (dbManager->isCacheEnabled(endpoint)) {
        YAML::Node deleteOperation;
        deleteOperation["summary"] = "Invalidate cache for " + endpoint.urlPath;
        deleteOperation["description"] = "Invalidates the cached data for this endpoint";
        
        // Define responses for DELETE
        deleteOperation["responses"]["200"]["description"] = "Cache successfully invalidated";
        deleteOperation["responses"]["500"]["description"] = "Internal server error while invalidating cache";
        
        // Add the same security requirements as the GET endpoint
        if (endpoint.auth.enabled) {
            deleteOperation["security"].push_back(YAML::Node());
            if (endpoint.auth.type == "basic") {
                deleteOperation["security"][0]["basicAuth"] = YAML::Node(YAML::NodeType::Sequence);
            } else {
                deleteOperation["security"][0]["bearerAuth"] = YAML::Node(YAML::NodeType::Sequence);
            }
        }

        pathItem["delete"] = deleteOperation;
    }

    return pathItem;
}

YAML::Node OpenAPIDocGenerator::generateParameters(const std::vector<RequestFieldConfig>& requestFields)
{
    YAML::Node parameters;
    
    for (const auto& field : requestFields) {
        YAML::Node parameter;
        parameter["name"] = field.fieldName;
        parameter["in"] = field.fieldIn;
        parameter["required"] = field.required;
        parameter["description"] = field.description;
        
        // Create schema node
        YAML::Node schema;
        schema["type"] = "string"; // Default type, could be improved by inferring from validators
        
        // Add default value if present
        if (!field.defaultValue.empty()) {
            schema["default"] = field.defaultValue;
        }
        
        parameter["schema"] = schema;
        parameters.push_back(parameter);
    }
    
    return parameters;
}

YAML::Node OpenAPIDocGenerator::generateResponseSchema(const EndpointConfig& endpoint)
{
    YAML::Node schema;
    
    YAML::Node properties = dbManager->describeSelectQuery(endpoint);
    
    if (endpoint.with_pagination) {
        schema["type"] = "object";
        schema["properties"]["data"]["type"] = "array";
        schema["properties"]["data"]["items"]["type"] = "object";
        schema["properties"]["data"]["items"]["properties"] = properties;
        
        schema["properties"]["next"]["type"] = "string";
        schema["properties"]["total_count"]["type"] = "integer";
    } else {
        schema["type"] = "array";
        schema["items"]["type"] = "object";
        schema["items"]["properties"] = properties;
    }
    
    return schema;
}

YAML::Node OpenAPIDocGenerator::generateConfigServiceDoc(crow::App<crow::CORSHandler, RateLimitMiddleware, AuthMiddleware>& app) {
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
            "Returns detailed performance and usage metrics for the Flapi server. "
            "Metrics include: request counts per endpoint, average response times, "
            "cache hit/miss rates, database query performance, rate limit statistics, "
            "authentication success/failure rates, and resource utilization. "
            "Use this for performance monitoring and optimization.",
            "Audit"
        );
        
        YAML::Node params = operation["parameters"];
        addParameter(params, "format", "query", "string", false,
            "Response format: 'json' (default) or 'prometheus' for Prometheus scraping");
        operation["parameters"] = params;
        
        YAML::Node responses = operation["responses"];
        addResponse(responses, "200",
            "Successful response with system metrics");
        addResponse(responses, "500",
            "Internal server error");
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