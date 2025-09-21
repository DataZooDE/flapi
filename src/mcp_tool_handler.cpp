#include "mcp_tool_handler.hpp"
#include <sstream>
#include <algorithm>

namespace flapi {

MCPToolHandler::MCPToolHandler(std::shared_ptr<DatabaseManager> db_manager,
                              std::shared_ptr<ConfigManager> config_manager)
    : db_manager(db_manager), config_manager(config_manager),
      validator(std::make_shared<RequestValidator>()),
      sql_processor(std::make_unique<SQLTemplateProcessor>(config_manager))
{
}

MCPToolExecutionResult MCPToolHandler::executeTool(const MCPToolCallRequest& request) {
    try {
        // Get the endpoint configuration by tool name
        const EndpointConfig* endpoint_config = getEndpointConfigByToolName(request.tool_name);
        if (!endpoint_config) {
            return createErrorResult("Tool not found: " + request.tool_name);
        }

        // Validate arguments
        if (!validateToolArguments(request.tool_name, request.arguments)) {
            return createErrorResult("Invalid arguments for tool: " + request.tool_name);
        }

        // Prepare parameters for SQL template
        std::map<std::string, std::string> params = prepareParameters(*endpoint_config, request.arguments);

        // Execute the query
        QueryResult query_result = executeQueryWithEndpoint(*endpoint_config, params);

        // QueryResult doesn't have success/error_message fields, assume success
        if (query_result.data.size() == 0) {
            return createErrorResult("Query returned no data");
        }

        // Format the result
        std::string result_format = endpoint_config->mcp_tool ? endpoint_config->mcp_tool->result_mime_type : "application/json";
        std::string formatted_result = formatResult(query_result, result_format);

        // Create metadata
        std::unordered_map<std::string, std::string> metadata;
        metadata["tool_name"] = request.tool_name;
        metadata["query_rows"] = std::to_string(query_result.data.size());
        metadata["execution_time_ms"] = "0"; // Simplified

        return createSuccessResult(formatted_result, metadata);
    } catch (const std::exception& e) {
        return createErrorResult("Tool execution error: " + std::string(e.what()));
    }
}

const EndpointConfig* MCPToolHandler::getEndpointConfigByToolName(const std::string& tool_name) const {
    const auto& endpoints = config_manager->getEndpoints();

    for (const auto& endpoint : endpoints) {
        if (endpoint.isMCPTool() && endpoint.mcp_tool->name == tool_name) {
            return &endpoint;
        }
    }

    return nullptr;
}

bool MCPToolHandler::validateToolArguments(const std::string& tool_name, const crow::json::wvalue& arguments) const {
    const EndpointConfig* endpoint_config = getEndpointConfigByToolName(tool_name);
    if (!endpoint_config) {
        return false;
    }

    // Convert JSON arguments to parameter map for validation
    std::map<std::string, std::string> params = convertJsonToParams(arguments);

    // Use RequestValidator to validate parameters
    std::vector<ValidationError> errors = validator->validateRequestParameters(endpoint_config->request_fields, params);

    if (!errors.empty()) {
        CROW_LOG_WARNING << "Tool argument validation failed for " << tool_name << ":";
        for (const auto& error : errors) {
            CROW_LOG_WARNING << "  - " << error.fieldName << ": " << error.errorMessage;
        }
        return false;
    }

    return true;
}

std::vector<std::string> MCPToolHandler::getAvailableTools() const {
    const auto& endpoints = config_manager->getEndpoints();
    std::vector<std::string> tool_names;

    for (const auto& endpoint : endpoints) {
        if (endpoint.isMCPTool()) {
            tool_names.push_back(endpoint.mcp_tool->name);
        }
    }

    return tool_names;
}

crow::json::wvalue MCPToolHandler::getToolDefinition(const std::string& tool_name) const {
    const EndpointConfig* endpoint_config = getEndpointConfigByToolName(tool_name);
    if (!endpoint_config) {
        return crow::json::wvalue();
    }

    crow::json::wvalue tool_def;
    tool_def["name"] = endpoint_config->mcp_tool->name;
    tool_def["description"] = endpoint_config->mcp_tool->description;
    tool_def["inputSchema"]["type"] = "object";

    // Build schema from request fields
    crow::json::wvalue properties;
    std::vector<std::string> required_fields;

    for (const auto& field : endpoint_config->request_fields) {
        crow::json::wvalue prop;
        prop["type"] = "string"; // Default type, could be enhanced based on validators
        prop["description"] = field.description;

        if (field.required) {
            required_fields.push_back(field.fieldName);
        }

        properties[field.fieldName] = std::move(prop);
    }

    tool_def["inputSchema"]["properties"] = std::move(properties);

    if (!required_fields.empty()) {
        tool_def["inputSchema"]["required"] = required_fields;
    }

    return tool_def;
}

std::map<std::string, std::string> MCPToolHandler::prepareParameters(const EndpointConfig& endpoint_config,
                                                                    const crow::json::wvalue& arguments) const {
    // Convert JSON arguments to parameter map
    std::map<std::string, std::string> params = convertJsonToParams(arguments);

    // Add any additional parameters from endpoint request fields that have defaults
    for (const auto& field : endpoint_config.request_fields) {
        if (field.defaultValue.empty() && params.find(field.fieldName) == params.end()) {
            // No default and no provided value - could add validation here
        }
    }

    return params;
}

QueryResult MCPToolHandler::executeQueryWithEndpoint(const EndpointConfig& endpoint_config,
                                                   std::map<std::string, std::string>& params) const {
    // Use the database manager to execute the query
    return db_manager->executeQuery(endpoint_config, params, true);
}

std::string MCPToolHandler::formatResult(const QueryResult& query_result,
                                       const std::string& format) const {
    // For now, return JSON format
    if (format == "application/json" || format.find("json") != std::string::npos) {
        return query_result.data.dump();
    } else {
        // Default to JSON
        return query_result.data.dump();
    }
}


std::map<std::string, std::string> MCPToolHandler::convertJsonToParams(const crow::json::wvalue& json_obj) const {
    std::map<std::string, std::string> params;

    if (json_obj.t() != crow::json::type::Object) {
        return params;
    }

    // Convert JSON object to parameter map using static_cast for proper type handling
    for (const auto& key : json_obj.keys()) {
        const auto& value = json_obj[key];
        params[key] = convertJsonValueToString(value);
    }

    return params;
}

std::string MCPToolHandler::convertJsonValueToString(const crow::json::wvalue& value) const {
    switch (value.t()) {
        case crow::json::type::String: {
            std::string str_value = value.dump();
            // Remove quotes if present
            if (str_value.length() >= 2 && str_value.front() == '"' && str_value.back() == '"') {
                str_value = str_value.substr(1, str_value.length() - 2);
            }
            return str_value;
        }

        case crow::json::type::Number:
            // Convert number to string representation without quotes
            return value.dump();  // This gives the numeric string representation

        case crow::json::type::True:
            return "true";

        case crow::json::type::False:
            return "false";

        case crow::json::type::Null:
            return "";  // Empty string for null values

        case crow::json::type::Object:
        case crow::json::type::List:
            // For complex types, return JSON representation
            return value.dump();

        default:
            // Fallback for unknown types
            return value.dump();
    }
}

MCPToolExecutionResult MCPToolHandler::createErrorResult(const std::string& error_message) const {
    MCPToolExecutionResult result;
    result.success = false;
    result.error_message = error_message;
    return result;
}

MCPToolExecutionResult MCPToolHandler::createSuccessResult(const std::string& result,
                                                          const std::unordered_map<std::string, std::string>& metadata) const {
    MCPToolExecutionResult execution_result;
    execution_result.success = true;
    execution_result.result = result;
    execution_result.metadata = metadata;
    return execution_result;
}


} // namespace flapi
