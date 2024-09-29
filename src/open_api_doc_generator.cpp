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
    //serverUrl += app.bindaddr().empty() ? "localhost" : app.bindaddr();
    serverUrl += "localhost";
    serverUrl += ":" + std::to_string(app.port());
    doc["servers"][0]["url"] = serverUrl;

    // Paths
    for (const auto& endpoint : configManager->getEndpoints()) {
        doc["paths"][endpoint.urlPath] = generatePathItem(endpoint);
    }

    return doc;
}

YAML::Node OpenAPIDocGenerator::generatePathItem(const EndpointConfig& endpoint)
{
    YAML::Node pathItem;
    
    std::string method = endpoint.method.empty() ? "get" : endpoint.method;
    YAML::Node operation;
    
    operation["summary"] = "Endpoint for " + endpoint.urlPath;
    operation["description"] = "Description not available";
    
    operation["parameters"] = generateParameters(endpoint.requestFields);
    
    operation["responses"]["200"]["description"] = "Successful response";
    operation["responses"]["200"]["content"]["application/json"]["schema"] = generateResponseSchema(endpoint);
    
    if (endpoint.rate_limit.enabled) {
        operation["x-rate-limit"]["max"] = endpoint.rate_limit.max;
        operation["x-rate-limit"]["interval"] = endpoint.rate_limit.interval;
    }
    
    if (endpoint.auth.enabled) {
        operation["security"][0]["bearerAuth"] = YAML::Node(YAML::NodeType::Sequence);
    }
    
    pathItem[method] = operation;
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
        parameter["schema"]["type"] = "string"; // You might want to infer the type based on validators
        parameters.push_back(parameter);
    }
    
    return parameters;
}

YAML::Node OpenAPIDocGenerator::generateResponseSchema(const EndpointConfig& endpoint)
{
    YAML::Node schema;
    
    YAML::Node properties = dbManager->describeSelectQuery(endpoint);
    
    schema["type"] = "object";
    schema["properties"]["data"]["type"] = "array";
    schema["properties"]["data"]["items"]["type"] = "object";
    schema["properties"]["data"]["items"]["properties"] = properties;
    
    schema["properties"]["next"]["type"] = "string";
    schema["properties"]["total_count"]["type"] = "integer";
    
    return schema;
}

} // namespace flapi