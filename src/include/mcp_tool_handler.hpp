#pragma once

#include <crow.h>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "config_manager.hpp"
#include "database_manager.hpp"
#include "sql_template_processor.hpp"
#include "request_validator.hpp"

namespace flapi {

struct MCPToolExecutionResult {
    bool success = false;
    std::string result;
    std::string error_message;
    std::unordered_map<std::string, std::string> metadata;
};

struct MCPToolCallRequest {
    std::string tool_name;
    crow::json::wvalue arguments;
    std::unordered_map<std::string, std::string> context;
};

class MCPToolHandler {
public:
    explicit MCPToolHandler(std::shared_ptr<DatabaseManager> db_manager,
                           std::shared_ptr<ConfigManager> config_manager);
    ~MCPToolHandler() = default;

    // Tool execution
    MCPToolExecutionResult executeTool(const MCPToolCallRequest& request);

    // Tool validation
    bool validateToolArguments(const std::string& tool_name, const crow::json::wvalue& arguments) const;

    // Tool discovery
    std::vector<std::string> getAvailableTools() const;
    crow::json::wvalue getToolDefinition(const std::string& tool_name) const;

private:
    // Helper methods to work with unified EndpointConfig
    const EndpointConfig* getEndpointConfigByToolName(const std::string& tool_name) const;

    // Tool execution helpers
    std::map<std::string, std::string> prepareParameters(const EndpointConfig& endpoint_config,
                                                        const crow::json::wvalue& arguments) const;
QueryResult executeQueryWithEndpoint(const EndpointConfig& endpoint_config,
                                   std::map<std::string, std::string>& params) const;
    std::string formatResult(const QueryResult& query_result,
                           const std::string& format) const;

    // Parameter conversion
    std::string convertJsonValueToString(const crow::json::wvalue& value) const;
    std::map<std::string, std::string> convertJsonToParams(const crow::json::wvalue& json_obj) const;

    // Error handling
    MCPToolExecutionResult createErrorResult(const std::string& error_message) const;
    MCPToolExecutionResult createSuccessResult(const std::string& result,
                                             const std::unordered_map<std::string, std::string>& metadata) const;

    std::shared_ptr<DatabaseManager> db_manager;
    std::shared_ptr<ConfigManager> config_manager;
    std::shared_ptr<RequestValidator> validator;
    std::unique_ptr<SQLTemplateProcessor> sql_processor;
};

} // namespace flapi
