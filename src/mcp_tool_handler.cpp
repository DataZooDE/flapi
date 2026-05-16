#include "mcp_tool_handler.hpp"
#include <chrono>
#include <sstream>
#include <algorithm>

namespace flapi {

MCPToolHandler::MCPToolHandler(std::shared_ptr<DatabaseManager> db_manager,
                              std::shared_ptr<ConfigManager> config_manager)
    : db_manager(db_manager), config_manager(config_manager),
      validator(std::make_shared<RequestValidator>()),
      sql_processor(std::make_unique<SQLTemplateProcessor>(config_manager)),
      audit_logger(config_manager->getAuditLogger())
{
}

MCPToolExecutionResult MCPToolHandler::executeTool(const MCPToolCallRequest& request) {
    const auto audit_started_at = std::chrono::steady_clock::now();
    const auto emit_audit = [&](const std::string& status, std::int64_t row_count) {
        if (!audit_logger || !audit_logger->isEnabled()) {
            return;
        }
        AuditEvent ev;
        ev.method = "tools/call";
        ev.target = request.tool_name;
        ev.status = status;
        ev.row_count = row_count;
        ev.latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - audit_started_at).count();
        auto principal_it = request.context.find("auth.username");
        if (principal_it != request.context.end() && !principal_it->second.empty()) {
            ev.principal = principal_it->second;
        }
        // Mirror the JSON arguments into the audit params map as strings; the
        // logger applies the configured redaction. We re-wrap each rvalue
        // child as a wvalue so .dump() handles all JSON types uniformly.
        if (request.arguments.t() == crow::json::type::Object) {
            auto parsed = crow::json::load(request.arguments.dump());
            if (parsed) {
                for (const auto& key : parsed.keys()) {
                    crow::json::wvalue tmp(parsed[key]);
                    std::string serialised = tmp.dump();
                    // Strip surrounding quotes for plain string values so the
                    // audit log doesn't contain visually doubled quoting.
                    if (serialised.size() >= 2 && serialised.front() == '"' && serialised.back() == '"') {
                        serialised = serialised.substr(1, serialised.size() - 2);
                    }
                    ev.params[key] = std::move(serialised);
                }
            }
        }
        audit_logger->log(std::move(ev));
    };

    try {
        // Get the endpoint configuration by tool name
        const EndpointConfig* endpoint_config = getEndpointConfigByToolName(request.tool_name);
        if (!endpoint_config) {
            emit_audit("error:tool_not_found", -1);
            return createErrorResult("Tool not found: " + request.tool_name);
        }

        // Per-tool RBAC check (W2.1). Runs before argument validation so a
        // denied caller never learns the parameter shape of a tool they
        // cannot invoke.
        {
            const bool mcp_auth_enabled = config_manager->getMCPConfig().auth.enabled;
            const std::vector<std::string> user_roles = parseRolesFromContext(request.context);
            const auto decision = authorization_policy.authorize(*endpoint_config, user_roles, mcp_auth_enabled);
            if (!decision.allowed) {
                CROW_LOG_WARNING << "MCP tool call denied for '" << request.tool_name
                                 << "': " << decision.reason;
                return createErrorResult("Permission denied: " + decision.reason);
            }
        }

        // Validate arguments
        if (!validateToolArguments(request.tool_name, request.arguments)) {
            emit_audit("error:invalid_arguments", -1);
            return createErrorResult("Invalid arguments for tool: " + request.tool_name);
        }

        // Prepare parameters for SQL template
        std::map<std::string, std::string> params = prepareParameters(*endpoint_config, request.arguments);

        // Check if this is a write operation
        if (endpoint_config->operation.type == OperationConfig::Write) {
            // Execute write operation
            WriteResult write_result;
            if (endpoint_config->operation.transaction) {
                write_result = db_manager->executeWriteInTransaction(*endpoint_config, params);
            } else {
                write_result = db_manager->executeWrite(*endpoint_config, params);
            }

            // Handle cache operations after successful write
            if (endpoint_config->cache.enabled) {
                if (endpoint_config->cache.invalidate_on_write) {
                    db_manager->invalidateCache(*endpoint_config);
                }
                // Note: refresh_on_write is handled by RequestHandler in REST context
                // For MCP, we just invalidate if configured
            }

            // Format write result
            crow::json::wvalue write_response;
            write_response["rows_affected"] = static_cast<int64_t>(write_result.rows_affected);
            
            if (write_result.returned_data.has_value() && endpoint_config->operation.returns_data) {
                write_response["data"] = std::move(*write_result.returned_data);
            } else if (write_result.returned_data.has_value()) {
                // Include returned data even if not explicitly configured
                write_response["data"] = std::move(*write_result.returned_data);
            }

            // Create metadata
            std::unordered_map<std::string, std::string> metadata;
            metadata["tool_name"] = request.tool_name;
            metadata["operation_type"] = "write";
            metadata["rows_affected"] = std::to_string(write_result.rows_affected);
            metadata["execution_time_ms"] = "0"; // Simplified

            emit_audit("success", static_cast<std::int64_t>(write_result.rows_affected));
            return createSuccessResult(write_response.dump(), metadata);
        } else {
            // Execute read query
        QueryResult query_result = executeQueryWithEndpoint(*endpoint_config, params);

        // Empty result sets are valid - they just mean no rows matched the criteria
        // Format the result (will be an empty array if no data)
        std::string result_format = endpoint_config->mcp_tool ? endpoint_config->mcp_tool->result_mime_type : "application/json";
        std::string formatted_result = formatResult(query_result, result_format);

        // Create metadata
        std::unordered_map<std::string, std::string> metadata;
        metadata["tool_name"] = request.tool_name;
        metadata["query_rows"] = std::to_string(query_result.data.size());
        metadata["execution_time_ms"] = "0"; // Simplified

        emit_audit("success", static_cast<std::int64_t>(query_result.data.size()));
        return createSuccessResult(formatted_result, metadata);
        }
    } catch (const std::exception& e) {
        emit_audit("error:exception", -1);
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

    // Use RequestValidator to validate known parameters
    std::vector<ValidationError> errors = validator->validateRequestParameters(endpoint_config->request_fields, params);

    // For MCP tools, always reject unknown parameters for security and correctness
    // This ensures that typos and invalid parameters are caught early
    auto unknownParamErrors = validator->validateRequestFields(endpoint_config->request_fields, params);
    errors.insert(errors.end(), unknownParamErrors.begin(), unknownParamErrors.end());

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

std::vector<std::string> MCPToolHandler::parseRolesFromContext(
    const std::unordered_map<std::string, std::string>& context)
{
    std::vector<std::string> roles;
    auto it = context.find(MCPToolCallRequest::kRolesContextKey);
    if (it == context.end() || it->second.empty()) {
        return roles;
    }

    const std::string& raw = it->second;
    std::string::size_type start = 0;
    while (start <= raw.size()) {
        const auto pos = raw.find(',', start);
        const auto end = (pos == std::string::npos) ? raw.size() : pos;
        const auto begin = raw.find_first_not_of(" \t", start);
        if (begin != std::string::npos && begin < end) {
            const auto trim_end = raw.find_last_not_of(" \t", end == 0 ? 0 : end - 1);
            if (trim_end != std::string::npos && trim_end >= begin) {
                roles.emplace_back(raw.substr(begin, trim_end - begin + 1));
            }
        }
        if (pos == std::string::npos) {
            break;
        }
        start = pos + 1;
    }
    return roles;
}


} // namespace flapi
