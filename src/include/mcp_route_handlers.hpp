#pragma once

#define CROW_ENABLE_COMPRESSION
#include <crow.h>
#include <chrono>
#include <fstream>
#include <iterator>
#include "crow/middlewares/cors.h"
#include "crow/compression.h"

#include "config_manager.hpp"
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
    void registerRoutes(crow::App<crow::CORSHandler, RateLimitMiddleware, AuthMiddleware>& app, int port = 8080);

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
    std::optional<EndpointConfig> findResourceByURI(const std::string& uri) const;
    crow::json::wvalue readResourceContent(const EndpointConfig& resource_config) const;

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

    // ========== Helper functions for reducing code duplication ==========

    // Creates a JSON-RPC error string: {"code":<code>,"message":"<message>"}
    static std::string formatJsonRpcError(int code, const std::string& message);

    // Initialize an MCPResponse with the request's ID
    static MCPResponse initResponse(const MCPRequest& request);

    // Validate a required string parameter exists and extract it
    // Returns true if valid, false if error (sets response.error)
    bool extractRequiredStringParam(const crow::json::wvalue& params,
                                    const std::string& param_name,
                                    std::string& out_value,
                                    MCPResponse& response) const;

    // Server state
    MCPServerInfo server_info_;
    MCPServerCapabilities capabilities_;
    std::vector<crow::json::wvalue> tool_definitions_;
    std::vector<crow::json::wvalue> resource_definitions_;
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
