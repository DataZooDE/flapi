#pragma once

#define CROW_ENABLE_COMPRESSION
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
#include "database_manager.hpp"
#include "mcp_tool_handler.hpp"
#include "mcp_types.hpp"
#include "mcp_constants.hpp"
#include "mcp_session_manager.hpp"
#include "mcp_client_capabilities.hpp"
#include "mcp_content_types.hpp"
#include "mcp_auth_handler.hpp"
#include "rate_limit_middleware.hpp"
#include "auth_middleware.hpp"
#include "config_tool_adapter.hpp"

namespace flapi {

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
    void registerRoutes(crow::App<crow::CORSHandler, FlapiCorsMiddleware, RateLimitMiddleware, AuthMiddleware>& app, int port = 8080);

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


    // Tool and resource discovery from unified configuration
    void discoverMCPEntities();
    std::vector<crow::json::wvalue> getToolDefinitions() const;
    std::vector<crow::json::wvalue> getResourceDefinitions() const;

    // Tool and resource discovery (const versions)
    void discoverMCPEntities() const;
    std::vector<crow::json::wvalue> getToolDefinitionsFromConfig() const;
    std::vector<crow::json::wvalue> getResourceDefinitionsFromConfig() const;

    // Implementation methods (internal)
    void discoverMCPEntitiesImpl();
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
    std::vector<crow::json::wvalue> tool_definitions_;
    std::vector<crow::json::wvalue> resource_definitions_;
    // Bumped on every refreshMCPEntities(); embedded in pagination cursors so a
    // cursor minted before a config reload is rejected rather than silently
    // paging over a changed list.
    std::atomic<uint64_t> entity_generation_{0};
    mutable std::mutex state_mutex_;

    // Dependencies
    std::shared_ptr<ConfigManager> config_manager_;
    std::shared_ptr<DatabaseManager> db_manager_;
    std::shared_ptr<MCPSessionManager> session_manager_;
    std::shared_ptr<MCPClientCapabilitiesDetector> capabilities_detector_;
    std::unique_ptr<MCPToolHandler> tool_handler_;
    std::unique_ptr<MCPAuthHandler> auth_handler_;
    std::unique_ptr<ConfigToolAdapter> config_tool_adapter_;
    int port_ = 8080;
};

} // namespace flapi
