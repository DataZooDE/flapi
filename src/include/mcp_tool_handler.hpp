#pragma once

#include <crow.h>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "audit_logger.hpp"
#include "config_manager.hpp"
#include "database_manager.hpp"
#include "mcp_authorization_policy.hpp"
#include "mcp_tool_rate_limiter.hpp"
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

    // Key used in `context` to pass the authenticated caller's roles
    // through to the tool handler as a comma-separated list. Kept as a
    // single string to keep the existing context map signature stable.
    static constexpr const char* kRolesContextKey = "auth.roles";
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

    // Parse `context[kRolesContextKey]` (comma-separated) into a role list.
    // Public so callers preparing an `MCPToolCallRequest` (and unit tests)
    // can use the same parsing rules as `executeTool` itself.
    static std::vector<std::string> parseRolesFromContext(
        const std::unordered_map<std::string, std::string>& context);

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
    std::shared_ptr<AuditLogger> audit_logger;
    MCPAuthorizationPolicy authorization_policy;
    MCPToolRateLimiter rate_limiter;
};

} // namespace flapi
