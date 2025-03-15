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

} // namespace flapi