#pragma once

#include <crow.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iterator>
#include "crow/middlewares/cors.h"
#include "crow/compression.h"

#include "config_manager.hpp"
#include "cors_middleware.hpp"
#include "flapi_app.hpp"
#include "database_manager.hpp"
#include "mcp_tool_handler.hpp"
#include "mcp_types.hpp"
#include "mcp_constants.hpp"
#include "mcp_session_manager.hpp"
#include "mcp_client_capabilities.hpp"
#include "mcp_content_types.hpp"
#include "mcp_auth_handler.hpp"
#include "mcp_task_manager.hpp"
#include "rate_limit_middleware.hpp"
#include "auth_middleware.hpp"
#include "config_tool_adapter.hpp"
#include "mcp_response_shaper.hpp"

namespace flapi {

/// The shaping resources/read applies: mcp-resource.response, else the same
/// file's mcp-tool.response, with max-rows defaulting to
/// MCPResourceInfo::kDefaultMaxRows. Never a no-op cap-wise.
ResponseShape resourceResponseShape(const EndpointConfig& endpoint);

/// What a successful tool result's text is, for its structuredContent.
///  - Rows: a JSON array of rows (or a write's object). structuredContent is
///    {"rows": <text>, "row_count": N}.
///  - Summary: an object that describes the call instead of returning rows (a
///    `sample: true` summary). structuredContent IS that object; it has no
///    `rows`, because there are none.
enum class ToolPayload { Rows, Summary };

/// The JSON text of a successful tools/call result: `result_text` as the text
/// block and, when it is JSON, as structuredContent too. The text is spliced
/// in, never re-parsed into a crow value (which re-orders row keys and
/// re-prints doubles).
std::string toolSuccessResultJson(const std::string& result_text, ToolPayload payload = ToolPayload::Rows);

/// The WWW-Authenticate value of a refused MCP request under `mcp.auth.type`
/// `auth_type`. `resource_metadata_url` is named (oidc only) when non-empty.
///  - basic: `Basic realm="flAPI"` on 401 (AuthMiddleware::challengeFor, as
///    REST answers); EMPTY on 403 (insufficient_scope), so no header is sent:
///    `error="insufficient_scope"` is an RFC 6750 Bearer term and Basic has
///    no scopes.
///  - bearer / oidc (and anything else): `Bearer[ resource_metadata="..."]`,
///    plus `error="insufficient_scope"` on 403.
std::string mcpWwwAuthenticate(const std::string& auth_type, const std::string& resource_metadata_url,
                               bool insufficient_scope);

/// completion/complete over an enum: the allowed values starting with
/// `prefix`, at most `kMaxValues` of them (the MCP spec's cap), `total` the
/// number of matches and `has_more` whether matches were left out.
struct CompletionValues {
    static constexpr std::size_t kMaxValues = 100;
    std::vector<std::string> values;
    std::size_t total = 0;
    bool has_more = false;
};
CompletionValues completeEnumValues(const std::vector<std::string>& allowed, const std::string& prefix);

/// The payload kind a tool handler's result metadata says it produced.
ToolPayload toolPayloadOf(const std::unordered_map<std::string, std::string>& metadata);

/**
 * MCPRouteHandlers provides HTTP route handlers for MCP (Model Context Protocol) endpoints.
 * These handlers can be registered with any Crow application to provide MCP functionality.
 */
class MCPRouteHandlers {
public:
    explicit MCPRouteHandlers(std::shared_ptr<ConfigManager> config_manager,
                             std::shared_ptr<DatabaseManager> db_manager,
                             std::shared_ptr<MCPSessionManager> session_manager,
                             std::shared_ptr<MCPClientCapabilitiesDetector> capabilities_detector,
                             std::unique_ptr<ConfigToolAdapter> config_tool_adapter = nullptr,
                             int port = 8080);

    ~MCPRouteHandlers() = default;

    /**
     * Register all MCP routes with the provided Crow application.
     * @param app The Crow application to register routes with
     * @param port The port number for the MCP server
     */
    void registerRoutes(flapi::FlapiApp& app, int port = 8080);

    /**
     * Refresh MCP entities from the configuration.
     * This can be called after the configuration has been fully loaded.
     */
    void refreshMCPEntities();

    /**
     * Get the server information for MCP protocol
     */
    MCPServerInfo getServerInfo() const { return server_info_; }

    /**
     * Get the server capabilities for MCP protocol
     */
    MCPServerCapabilities getServerCapabilities() const { return capabilities_; }

private:
    // HTTP route handlers
    crow::response handleInitialize(const crow::request& req);
    crow::response handleToolsList(const crow::request& req);
    crow::response handleToolsCall(const crow::request& req);
    crow::response handleResourcesList(const crow::request& req);
    crow::response handleResourcesRead(const crow::request& req);
    crow::response handleHealth(const crow::request& req);

    // JSON-RPC message handling
    MCPResponse handleMessage(const MCPRequest& request, const crow::request& http_req) const;
    MCPResponse handleInitializeRequest(const MCPRequest& request, const crow::request& http_req) const;
    MCPResponse handleToolsListRequest(const MCPRequest& request, const crow::request& http_req) const;
    MCPResponse handleToolsCallRequest(const MCPRequest& request, const crow::request& http_req) const;
    MCPResponse handleResourcesListRequest(const MCPRequest& request, const crow::request& http_req) const;
    MCPResponse handleResourcesReadRequest(const MCPRequest& request, const crow::request& http_req) const;

    // Request parsing and validation
    std::optional<MCPRequest> parseMCPRequest(const crow::request& req) const;
    MCPRequest extractRequestFields(const crow::json::wvalue& json_request) const;
    bool validateMCPRequest(const MCPRequest& request) const;

    // Response creation
    crow::response createJsonRpcResponse(const MCPRequest& request, const MCPResponse& mcp_response,
                                         const std::optional<std::string>& session_id = std::nullopt) const;
    crow::response createJsonRpcErrorResponse(const std::string& id, int code, const std::string& message,
                                              const std::optional<std::string>& session_id = std::nullopt) const;

    // Session management
    std::optional<std::string> extractSessionIdFromRequest(const crow::request& req) const;
    void addSessionHeaderToResponse(crow::response& resp, const std::optional<std::string>& session_id) const;

    // Request dispatching
    MCPResponse dispatchMCPRequest(const MCPRequest& request, const crow::request& http_req) const;


    // Tool and resource discovery from unified configuration. A single
    // implementation (discoverMCPEntitiesImpl) populates tool_definitions_ /
    // resource_definitions_; callers read them via the *FromConfig accessors.
    //
    // The definitions are rebuilt whenever the endpoint table changes. Every
    // mutation (create/update/delete over REST or MCP, reload, filesystem
    // reload) publishes a NEW copy-on-write snapshot, so comparing the pinned
    // snapshot against the one the definitions were built from catches all of
    // them without each mutation site having to remember to notify MCP.
    void discoverMCPEntitiesImpl() const;
    // Rediscover iff the endpoint snapshot changed since the last discovery.
    void ensureEntitiesFresh() const;
    std::vector<crow::json::wvalue> getToolDefinitionsFromConfig() const;
    std::vector<crow::json::wvalue> getResourceDefinitionsFromConfig() const;
    std::vector<crow::json::wvalue> getToolDefinitionsImpl() const;
    std::vector<crow::json::wvalue> getResourceDefinitionsImpl() const;

    // Helper to convert unified endpoint config to MCP tool/resource definitions
    crow::json::wvalue endpointToMCPToolDefinition(const EndpointConfig& endpoint) const;
    crow::json::wvalue endpointToMCPResourceDefinition(const EndpointConfig& endpoint) const;
    crow::json::wvalue endpointToMCPPromptDefinition(const EndpointConfig& endpoint) const;

    // Resource reading functionality
    MCPResponse handleResourcesTemplatesListRequest(const MCPRequest& request, const crow::request& http_req) const;
    // Resolve a resources/read URI to its endpoint. Tries an exact static match
    // (flapi://<name>) first, then any resource whose uri-template matches,
    // binding the template's {var} path segments into `bound_params`.
    std::optional<EndpointConfig> findResourceByURI(const std::string& uri,
                                                    std::map<std::string, std::string>& bound_params) const;
    crow::json::wvalue readResourceContent(const EndpointConfig& resource_config,
                                           const std::map<std::string, std::string>& params) const;

    // Prompt functionality
    MCPResponse handlePromptsListRequest(const MCPRequest& request, const crow::request& http_req) const;
    MCPResponse handlePromptsGetRequest(const MCPRequest& request, const crow::request& http_req) const;
    std::optional<EndpointConfig> findPromptByName(const std::string& name) const;
    crow::json::wvalue processPromptTemplate(const EndpointConfig& prompt_config, const crow::json::wvalue* arguments) const;

    // Logging functionality (2025-11-25)
    MCPResponse handleLoggingSetLevelRequest(const MCPRequest& request, const crow::request& http_req) const;

    // Completion functionality (2025-11-25)
    MCPResponse handleCompletionCompleteRequest(const MCPRequest& request, const crow::request& http_req) const;

    // Ping functionality
    MCPResponse handlePingRequest(const MCPRequest& request, const crow::request& http_req) const;

    // MCP 2026-07-28 discovery: returns supportedVersions, capabilities,
    // instructions and serverInfo statelessly (the modern replacement for
    // initialize). Must be reachable without a session.
    MCPResponse handleServerDiscoverRequest(const MCPRequest& request, const crow::request& http_req) const;

    // ========== Helper functions for reducing code duplication ==========

    // Creates a JSON-RPC error string: {"code":<code>,"message":"<message>"}
    static std::string formatJsonRpcError(int code, const std::string& message);

    // Initialize an MCPResponse with the request's ID
    static MCPResponse initResponse(const MCPRequest& request);

    // Layer-2 per-entity RBAC for resources and prompts (mirrors the per-tool
    // policy applied inside MCPToolHandler). Derives the caller's roles from the
    // HTTP request and applies MCPAuthorizationPolicy against the entity's
    // allowed-roles. Returns std::nullopt when access is allowed, or a
    // human-readable denial reason when it is not. `entity_label` is used only
    // in the reason string (e.g. "Resource 'customer_schema'").
    std::optional<std::string> authorizeMCPEntity(
        const crow::request& http_req,
        const std::optional<std::vector<std::string>>& allowed_roles,
        const std::string& entity_label) const;

    // MCP 2026-07-28 mirrored-header validation (modern era only). Checks the
    // MCP-Protocol-Version / Mcp-Method / Mcp-Name headers against the request
    // body (with base64-sentinel decoding and numeric equality). Returns a
    // human-readable mismatch reason, or std::nullopt when all present/required
    // headers agree with the body.
    std::optional<std::string> validateMirroredHeaders(const crow::request& http_req,
                                                       const MCPRequest& request) const;

    // RFC 9728: the absolute URL of this server's protected-resource metadata
    // document, derived from the forwarded/Host headers (or the configured
    // canonical resource URI). Used in the WWW-Authenticate challenge.
    std::string buildResourceMetadataUrl(const crow::request& http_req) const;

    // The `WWW-Authenticate` header value for an auth challenge. When
    // `insufficient_scope` is true it is a 403 authorization failure
    // (`error="insufficient_scope"`); otherwise a 401 authentication challenge.
    // Includes `resource_metadata="<url>"` only when OIDC is configured.
    std::string buildWwwAuthenticate(const crow::request& http_req, bool insufficient_scope) const;

    // Validate a required string parameter exists and extract it
    // Returns true if valid, false if error (sets response.error)
    bool extractRequiredStringParam(const crow::json::wvalue& params,
                                    const std::string& param_name,
                                    std::string& out_value,
                                    MCPResponse& response) const;

    // Applies cursor-based pagination to a list result. When mcp.page-size is 0
    // (default) the whole list is returned and out_next_cursor is left empty —
    // exactly the pre-pagination behaviour. Otherwise it decodes params.cursor
    // (base64 {offset,gen}; a stale generation or malformed cursor sets
    // response.error and returns false), slices one page, and sets
    // out_next_cursor when more items remain. `total` is the full item count.
    bool applyPagination(const MCPRequest& request, size_t total,
                         size_t& out_offset, size_t& out_count,
                         std::string& out_next_cursor, MCPResponse& response) const;

    // Server state
    MCPServerInfo server_info_;
    MCPServerCapabilities capabilities_;
    // Mutable: rebuilt lazily from const request handlers (ensureEntitiesFresh).
    mutable std::vector<crow::json::wvalue> tool_definitions_;
    mutable std::vector<crow::json::wvalue> resource_definitions_;
    // The endpoint snapshot the definitions above were built from. Held (not
    // just its address) so the vector cannot be freed and a new snapshot
    // allocated at the same address, which would hide a change.
    mutable std::shared_ptr<const std::vector<EndpointConfig>> discovered_snapshot_;
    // Bumped on every rediscovery; embedded in pagination cursors so a cursor
    // minted before a config change is rejected rather than silently paging
    // over a changed list.
    mutable std::atomic<uint64_t> entity_generation_{0};
    mutable std::mutex state_mutex_;

    // Per-tool outputSchema learned from the first successful tools/call result
    // (flAPI cannot know a parameterised query's result columns statically, so
    // the schema is derived from real data and cached, then merged into
    // tools/list). Cleared on refreshMCPEntities().
    mutable std::mutex output_schema_mu_;
    mutable std::unordered_map<std::string, std::string> output_schema_cache_;
    // Infer and cache an outputSchema (JSON Schema string) for `tool_name` from
    // the serialized row array `rows_json`, if not already cached.
    void cacheOutputSchemaFromRows(const std::string& tool_name, const std::string& rows_json) const;
    std::string getCachedOutputSchema(const std::string& tool_name) const;

    // Dependencies
    std::shared_ptr<ConfigManager> config_manager_;
    std::shared_ptr<DatabaseManager> db_manager_;
    std::shared_ptr<MCPSessionManager> session_manager_;
    std::shared_ptr<MCPClientCapabilitiesDetector> capabilities_detector_;
    std::unique_ptr<MCPToolHandler> tool_handler_;
    std::unique_ptr<MCPAuthHandler> auth_handler_;
    std::unique_ptr<ConfigToolAdapter> config_tool_adapter_;
    std::unique_ptr<MCPTaskManager> task_manager_;
    int port_ = 8080;

    // Tasks extension handlers (modern era only).
    MCPResponse handleTasksGetRequest(const MCPRequest& request, const crow::request& http_req) const;
    MCPResponse handleTasksCancelRequest(const MCPRequest& request, const crow::request& http_req) const;
    // True when the modern client declared the io.modelcontextprotocol/tasks
    // extension; a task is never surfaced to a client that cannot poll it.
    static bool clientSupportsTasks(const MCPRequest& request);
    // Serialize a task snapshot into the MCP task result object.
    crow::json::wvalue taskToJson(const MCPTaskManager::Task& task) const;
};

} // namespace flapi
