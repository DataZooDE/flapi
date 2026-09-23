#pragma once

#include <crow.h>
#include <memory>
#include <string>
#include <unordered_map>
#include <map>
#include <optional>
#include <vector>

#include "audit_logger.hpp"
#include "config_manager.hpp"
#include "database_manager.hpp"
#include "mcp_types.hpp"
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
        ServiceUnavailable,// cache not ready         -> JSON-RPC 503
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
    static constexpr const char* kUsernameContextKey = "auth.username";
    static constexpr const char* kAuthTypeContextKey = "auth.type";
    static constexpr const char* kEmailContextKey = "auth.email";

    /// True once the caller has been authenticated. Set alongside the keys
    /// above so a tool can distinguish "anonymous" from "authenticated with an
    /// empty username" - the two render identically otherwise.
    static constexpr const char* kAuthenticatedContextKey = "auth.authenticated";
};

/// Build the transport-neutral auth-context map from an authenticated MCP
/// session. The one place the key names are chosen.
std::unordered_map<std::string, std::string> mcpAuthContextFrom(
    const std::optional<MCPSession::AuthContext>& auth);

/// Strip caller-supplied `__auth_*` from `params`, then inject the identity
/// the transport authenticated.
///
/// BOTH halves, in ONE function, because doing only one of them is a
/// vulnerability either way: stripping without injecting leaves `auth.*`
/// empty, so the documented `{{#auth.username}}WHERE tenant = ...{{/...}}`
/// filter renders NOTHING and returns every tenant's rows; injecting without
/// stripping lets the caller declare who it is.
///
/// And in one function because the first fix did this inline in
/// MCPToolHandler::prepareParameters, which is tools/call only - resources/read
/// passed its bound URI-template params straight to executeQuery, so the
/// cross-tenant disclosure survived one protocol method over.
void applyMcpAuthContext(std::map<std::string, std::string>& params,
                         const std::unordered_map<std::string, std::string>& context);

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

    // True only for a tool in the configured endpoint set. Callers must gate
    // any use of a caller-supplied tool name on this: the name reaches the span
    // name and gen_ai.tool.name at the DEFAULT capture tier, so an unvalidated
    // one is both exported content and an unbounded metric dimension.
    bool isKnownTool(const std::string& tool_name) const;

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
    /// Fill in `default:` values for arguments the caller omitted, before
    /// validation runs - matching the order the REST path uses.
    void applyDefaultArguments(const EndpointConfig& endpoint_config,
                               crow::json::wvalue& arguments) const;

    /// `context` carries the identity the transport authenticated. It is
    /// injected as `__auth_*` AFTER the caller's own `__auth_*` keys are
    /// stripped, which is what makes `auth.username` and friends usable from
    /// an MCP template at all. Passing an empty map yields the anonymous
    /// context, which is what the unauthenticated MCP default produces.
    std::map<std::string, std::string> prepareParameters(
        const EndpointConfig& endpoint_config,
        const crow::json::wvalue& arguments,
        const std::unordered_map<std::string, std::string>& context = {}) const;
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
