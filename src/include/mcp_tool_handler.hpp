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
    // Classifies a failure so the transport layer can choose between a JSON-RPC
    // protocol error (things the model cannot fix — bad tool name, denied
    // access) and a tool result with `isError: true` (things the model can
    // self-correct — bad arguments, a SQL/runtime error, a rate limit).
    enum class FailureKind {
        None,
        NotFound,          // unknown tool            -> JSON-RPC -32602
        PermissionDenied,  // RBAC denial             -> JSON-RPC error (403 later)
        RateLimited,       // per-tool rate limit hit -> isError result
        InvalidArguments,  // validation failed       -> isError result
        ExecutionError,    // SQL/runtime failure     -> isError result
    };

    bool success = false;
    FailureKind failure_kind = FailureKind::None;
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

    // Tool execution. Thin wrapper that times the call and emits the
    // `mcp_tool_called` telemetry event (bounded tool name + status + duration
    // only); the real work lives in executeToolImpl.
    MCPToolExecutionResult executeTool(const MCPToolCallRequest& request);

    // Tool validation. The overload with `error_out` joins the per-field
    // validator messages (field: reason; ...) so the caller can surface them to
    // the model in an isError result instead of a generic "invalid arguments".
    bool validateToolArguments(const std::string& tool_name, const crow::json::wvalue& arguments) const;
    bool validateToolArguments(const std::string& tool_name, const crow::json::wvalue& arguments,
                               std::string& error_out) const;

    // Tool discovery
    std::vector<std::string> getAvailableTools() const;
    crow::json::wvalue getToolDefinition(const std::string& tool_name) const;

    // Parse `context[kRolesContextKey]` (comma-separated) into a role list.
    // Public so callers preparing an `MCPToolCallRequest` (and unit tests)
    // can use the same parsing rules as `executeTool` itself.
    static std::vector<std::string> parseRolesFromContext(
        const std::unordered_map<std::string, std::string>& context);

private:
    // Real tool execution body (wrapped by executeTool for telemetry).
    MCPToolExecutionResult executeToolImpl(const MCPToolCallRequest& request);

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
    MCPToolExecutionResult createErrorResult(
        const std::string& error_message,
        MCPToolExecutionResult::FailureKind kind =
            MCPToolExecutionResult::FailureKind::ExecutionError) const;
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
