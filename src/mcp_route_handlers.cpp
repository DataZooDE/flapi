#include "mcp_route_handlers.hpp"
#include "json_utils.hpp"
#include <iostream>
#include <sstream>
#include <optional>

// Windows headers define DEBUG, ERROR, INFO, WARNING as macros which conflict
// with Crow's LogLevel enum values. Undefine them here.
#ifdef _WIN32
#undef DEBUG
#undef INFO
#undef WARNING
#undef ERROR
#endif

namespace flapi {

// ========== Helper function implementations ==========

std::string MCPRouteHandlers::formatJsonRpcError(int code, const std::string& message) {
    // Escape quotes in message for JSON safety
    std::string escaped_message;
    escaped_message.reserve(message.size());
    for (char c : message) {
        if (c == '"') {
            escaped_message += "\\\"";
        } else if (c == '\\') {
            escaped_message += "\\\\";
        } else if (c == '\n') {
            escaped_message += "\\n";
        } else if (c == '\r') {
            escaped_message += "\\r";
        } else if (c == '\t') {
            escaped_message += "\\t";
        } else {
            escaped_message += c;
        }
    }
    return "{\"code\":" + std::to_string(code) + ",\"message\":\"" + escaped_message + "\"}";
}

MCPResponse MCPRouteHandlers::initResponse(const MCPRequest& request) {
    MCPResponse response;
    response.id = request.id;
    return response;
}

bool MCPRouteHandlers::extractRequiredStringParam(const crow::json::wvalue& params,
                                                   const std::string& param_name,
                                                   std::string& out_value,
                                                   MCPResponse& response) const {
    // Check if parameter exists
    if (params.count(param_name) == 0) {
        response.error = formatJsonRpcError(-32602, "Invalid params: missing " + param_name);
        return false;
    }

    auto param_value = params[param_name];

    // Check for null
    if (JsonUtils::isNull(param_value)) {
        response.error = formatJsonRpcError(-32602, "Invalid params: " + param_name + " cannot be null");
        return false;
    }

    // Extract string value
    if (JsonUtils::isString(param_value)) {
        out_value = JsonUtils::extractString(param_value);
    } else {
        out_value = param_value.dump();
    }

    return true;
}

// ========== End helper functions ==========

MCPRouteHandlers::MCPRouteHandlers(std::shared_ptr<ConfigManager> config_manager,
                                 std::shared_ptr<DatabaseManager> db_manager,
                                 std::shared_ptr<MCPSessionManager> session_manager,
                                 std::shared_ptr<MCPClientCapabilitiesDetector> capabilities_detector,
                                 int port)
    : config_manager_(config_manager), db_manager_(db_manager),
      session_manager_(session_manager), capabilities_detector_(capabilities_detector),
      port_(port)
{
    CROW_LOG_INFO << "MCPRouteHandlers constructor called - initializing MCP server components";
    // Initialize tool handler
    try {
        tool_handler_ = std::make_unique<MCPToolHandler>(db_manager, config_manager);
        CROW_LOG_DEBUG << "MCPToolHandler initialized successfully";
    } catch (const std::exception& e) {
        CROW_LOG_ERROR << "Failed to initialize MCPToolHandler: " << e.what();
        tool_handler_ = nullptr;
    }

    // Initialize MCP auth handler
    try {
        auth_handler_ = std::make_unique<MCPAuthHandler>(config_manager);
        CROW_LOG_DEBUG << "MCPAuthHandler initialized successfully";
    } catch (const std::exception& e) {
        CROW_LOG_ERROR << "Failed to initialize MCPAuthHandler: " << e.what();
        auth_handler_ = nullptr;
    }

    // Initialize server info with default values (no separate MCP config needed)
    server_info_.name = "flapi-mcp-server";
    server_info_.version = "0.3.0";
    server_info_.protocol_version = "2025-11-25";

    // Initialize server capabilities with all available capabilities
    capabilities_.tools = {"tools", "resources"};
    capabilities_.resources = {"resources"};

    // Discover MCP entities from unified configuration
    // Note: This might fail if endpoints are not yet loaded. We'll retry later if needed.
    try {
        discoverMCPEntities();
        CROW_LOG_DEBUG << "MCP entities discovered successfully: " << tool_definitions_.size() << " tools, " << resource_definitions_.size() << " resources";
    } catch (const std::exception& e) {
        CROW_LOG_WARNING << "Failed to discover MCP entities during construction: " << e.what();
        CROW_LOG_DEBUG << "Will retry MCP entity discovery when routes are registered";
    }

    CROW_LOG_INFO << "MCP Route Handlers initialized with " << tool_definitions_.size() << " tools and "
                  << resource_definitions_.size() << " resources";
    CROW_LOG_INFO << "flAPI MCP server ready at: http://localhost:" << (port_ > 0 ? std::to_string(port_) : "8080") << "/mcp/jsonrpc";
    CROW_LOG_INFO << "Transport type: Streamable HTTP, URL ready to paste into MCP inspector tool";
}

void MCPRouteHandlers::registerRoutes(crow::App<crow::CORSHandler, RateLimitMiddleware, AuthMiddleware>& app, int port) {
    port_ = port; // Update port if provided

    CROW_LOG_INFO << "Registering MCP routes with application...";
    CROW_LOG_INFO << "MCP Route Handlers registerRoutes called - tools: " << tool_definitions_.size() << ", resources: " << resource_definitions_.size();

    // MCP JSON-RPC endpoint (main protocol endpoint)
    CROW_ROUTE(app, "/mcp/jsonrpc")
        .methods("POST"_method)
        ([this](const crow::request& req) -> crow::response {
            try {
                CROW_LOG_DEBUG << "MCP JSON-RPC route handler called";

                // Extract session ID from request (if present)
                auto session_id = extractSessionIdFromRequest(req);

                // Parse and validate the request EARLY to determine if it's initialize
                auto mcp_request = parseMCPRequest(req);
                if (!mcp_request) {
                    CROW_LOG_ERROR << "Failed to parse MCP request";
                    return createJsonRpcErrorResponse("", -32700, "Parse error: Invalid JSON", session_id);
                }

                CROW_LOG_DEBUG << "MCP request: method=" << mcp_request->method << ", id=" << mcp_request->id;

                // For initialize requests, perform Layer 1 (protocol) auth BEFORE creating session
                std::optional<MCPSession::AuthContext> auth_context;
                if (mcp_request->method == "initialize") {
                    if (auth_handler_ && auth_handler_->methodRequiresAuth("initialize")) {
                        auth_context = auth_handler_->authenticate(req);
                        if (!auth_context) {
                            CROW_LOG_WARNING << "MCP initialize: authentication required but failed";
                            MCPResponse mcp_response;
                            mcp_response.id = mcp_request->id;
                            mcp_response.error = "{\"code\":-32001,\"message\":\"Authentication required for initialize\"}";
                            return createJsonRpcResponse(*mcp_request, mcp_response, std::nullopt);
                        }
                    }
                }

                // Create or reuse session
                if (!session_id && session_manager_) {
                    // New session: create with auth context (if authenticated)
                    session_id = session_manager_->createSession("", auth_context);
                    CROW_LOG_INFO << "Created new session: " << session_id.value();
                } else if (session_id) {
                    CROW_LOG_DEBUG << "Session ID extracted from request: " << session_id.value();
                    // Update activity timestamp for existing session
                    if (session_manager_) {
                        session_manager_->updateSessionActivity(session_id.value());
                    }

                    // For non-initialize methods on existing session: check session auth
                    if (mcp_request->method != "initialize") {
                        auto session = session_manager_->getSession(session_id.value());
                        if (session) {
                            auth_context = session->auth_context;
                        }

                        // Check method authorization (Layer 1: Protocol auth)
                        if (auth_handler_ && !auth_handler_->authorizeMethod(mcp_request->method, auth_context)) {
                            CROW_LOG_WARNING << "MCP method " << mcp_request->method << " requires authentication";
                            MCPResponse mcp_response;
                            mcp_response.id = mcp_request->id;
                            mcp_response.error = "{\"code\":-32001,\"message\":\"Authentication required for method: " +
                                               mcp_request->method + "\"}";
                            return createJsonRpcResponse(*mcp_request, mcp_response, session_id);
                        }
                    }
                } else {
                    // New session without auth (should not happen for non-initialize, but handle gracefully)
                    if (session_manager_) {
                        session_id = session_manager_->createSession("", auth_context);
                        CROW_LOG_INFO << "Created new session: " << session_id.value();
                    }
                }

                // Dispatch the request to appropriate handler (passing HTTP request for auth access)
                MCPResponse mcp_response = dispatchMCPRequest(*mcp_request, req);

                // Create and return JSON-RPC response with session header
                return createJsonRpcResponse(*mcp_request, mcp_response, session_id);
            } catch (const std::exception& e) {
                CROW_LOG_ERROR << "Error handling MCP request: " << e.what();
                return createJsonRpcErrorResponse("", -32603, "Internal JSON-RPC error: " + std::string(e.what()), std::nullopt);
            }
        });

    // Health check endpoint
    CROW_ROUTE(app, "/mcp/health")
        .methods("GET"_method)
        ([this]() -> crow::response {
            crow::json::wvalue health;
            health["status"] = "healthy";
            health["server"] = server_info_.name;
            health["version"] = server_info_.version;
            health["protocol_version"] = server_info_.protocol_version;
            auto tool_defs = getToolDefinitionsFromConfig();
            auto resource_defs = getResourceDefinitionsFromConfig();
            health["mcp_available"] = true;
            health["tools_available"] = (tool_defs.size() > 0);
            health["resources_available"] = (resource_defs.size() > 0);
            health["tools_count"] = static_cast<int>(tool_defs.size());
            health["resources_count"] = static_cast<int>(resource_defs.size());
            return crow::response(200, health);
        });

    // MCP session cleanup endpoint (DELETE request to close session)
    CROW_ROUTE(app, "/mcp/jsonrpc")
        .methods("DELETE"_method)
        ([this](const crow::request& req) -> crow::response {
            try {
                CROW_LOG_DEBUG << "MCP session cleanup endpoint called";

                // Extract session ID from request
                auto session_id = extractSessionIdFromRequest(req);
                if (!session_id) {
                    CROW_LOG_WARNING << "DELETE /mcp/jsonrpc called without session ID header";
                    crow::json::wvalue error_response;
                    error_response["jsonrpc"] = "2.0";
                    error_response["id"] = nullptr;
                    error_response["error"]["code"] = -32000;
                    error_response["error"]["message"] = "Missing Mcp-Session-Id header for session cleanup";
                    return crow::response(400, error_response.dump());
                }

                CROW_LOG_INFO << "Cleaning up session: " << session_id.value();

                // Notify session manager to cleanup the session
                if (session_manager_) {
                    session_manager_->removeSession(session_id.value());
                    CROW_LOG_DEBUG << "Session removed by session manager: " << session_id.value();
                }

                // Return success response
                crow::json::wvalue success_response;
                success_response["jsonrpc"] = "2.0";
                success_response["id"] = nullptr;
                success_response["result"]["session_id"] = session_id.value();
                success_response["result"]["status"] = "closed";

                auto response = crow::response(200, success_response.dump());
                response.set_header("Content-Type", "application/json");
                addSessionHeaderToResponse(response, session_id);
                return response;
            } catch (const std::exception& e) {
                CROW_LOG_ERROR << "Error handling MCP session cleanup: " << e.what();
                crow::json::wvalue error_response;
                error_response["jsonrpc"] = "2.0";
                error_response["id"] = nullptr;
                error_response["error"]["code"] = -32603;
                error_response["error"]["message"] = std::string("Internal error during session cleanup: ") + e.what();
                return crow::response(500, error_response.dump());
            }
        });

    // Try to refresh MCP entities now that configuration should be loaded
    refreshMCPEntities();

    CROW_LOG_INFO << "MCP routes registered with application";
}

void MCPRouteHandlers::refreshMCPEntities() {
    try {
        discoverMCPEntities();
        CROW_LOG_INFO << "MCP entities refreshed: " << tool_definitions_.size() << " tools, " << resource_definitions_.size() << " resources";
    } catch (const std::exception& e) {
        CROW_LOG_WARNING << "Failed to refresh MCP entities: " << e.what();
        CROW_LOG_DEBUG << "Continuing with existing MCP definitions";
    }
}

// Request parsing and validation methods
std::optional<MCPRequest> MCPRouteHandlers::parseMCPRequest(const crow::request& req) const {
    // Parse JSON request
    auto json_request = crow::json::load(req.body);
    if (!json_request) {
        CROW_LOG_ERROR << "Invalid JSON in MCP request: " << req.body;
        return std::nullopt;
    }

    // Extract fields from JSON
    MCPRequest mcp_request = extractRequestFields(json_request);

    // Validate the request
    if (!validateMCPRequest(mcp_request)) {
        return std::nullopt;
    }

    CROW_LOG_DEBUG << "parseMCPRequest returning method: '" << mcp_request.method << "'";

    return mcp_request;
}

MCPRequest MCPRouteHandlers::extractRequestFields(const crow::json::wvalue& json_request) const {
    MCPRequest mcp_req;

    // Handle params field - copy the JSON value
    if (json_request.count("params") > 0) {
        mcp_req.params = crow::json::wvalue(json_request["params"]);
    } else {
        mcp_req.params = crow::json::wvalue::object();
    }

    // Handle jsonrpc field which should be "2.0"
    auto jsonrpc_value = json_request["jsonrpc"];
    if (JsonUtils::isString(jsonrpc_value)) {
        mcp_req.jsonrpc = JsonUtils::extractString(jsonrpc_value);
    } else if (JsonUtils::isNull(jsonrpc_value)) {
        mcp_req.jsonrpc = "2.0";  // Default to 2.0 for null
    } else {
        // Convert to string using dump()
        mcp_req.jsonrpc = jsonrpc_value.dump();
    }

    // Handle method field which should be a string
    auto method_value = json_request["method"];
    if (JsonUtils::isString(method_value)) {
        mcp_req.method = JsonUtils::extractString(method_value);
    } else if (JsonUtils::isNull(method_value)) {
        mcp_req.method = "";  // Will be caught by validation
    } else {
        // Convert to string using dump()
        mcp_req.method = method_value.dump();
    }

    // Handle id field which can be string, number, or null
    auto id_value = json_request["id"];
    if (JsonUtils::isString(id_value)) {
        // Extract string value without quotes
        mcp_req.id = JsonUtils::extractString(id_value);
    } else if (JsonUtils::isNumber(id_value)) {
        // Convert number to string using dump()
        mcp_req.id = id_value.dump();
    } else if (JsonUtils::isNull(id_value)) {
        // Use empty string for null id
        mcp_req.id = "";
    } else {
        // Fallback for other types - convert to string representation using dump
        mcp_req.id = id_value.dump();
    }

    return mcp_req;
}

bool MCPRouteHandlers::validateMCPRequest(const MCPRequest& request) const {
    // Check if method is empty (was null)
    if (request.method.empty()) {
        CROW_LOG_ERROR << "Invalid Request: method cannot be null";
        return false;
    }

    // Basic validation - method should not be empty and should be valid
    if (request.method.empty()) {
        CROW_LOG_ERROR << "Invalid Request: method cannot be empty";
        return false;
    }

    return true;
}

MCPResponse MCPRouteHandlers::dispatchMCPRequest(const MCPRequest& request, const crow::request& http_req) const {
    return handleMessage(request, http_req);
}

std::optional<std::string> MCPRouteHandlers::extractSessionIdFromRequest(const crow::request& req) const {
    // Try to get session ID from Mcp-Session-Id header
    auto session_header = req.get_header_value(flapi::mcp::constants::MCP_SESSION_HEADER);
    if (!session_header.empty()) {
        return session_header;
    }
    return std::nullopt;
}

void MCPRouteHandlers::addSessionHeaderToResponse(crow::response& resp,
                                                   const std::optional<std::string>& session_id) const {
    if (session_id) {
        resp.set_header(flapi::mcp::constants::MCP_SESSION_HEADER, session_id.value());
        CROW_LOG_DEBUG << "Added session header to response: " << session_id.value();
    }
}

crow::response MCPRouteHandlers::createJsonRpcResponse(const MCPRequest& request, const MCPResponse& mcp_response,
                                                       const std::optional<std::string>& session_id) const {
    crow::json::wvalue response_json;
    response_json["jsonrpc"] = "2.0";

    // Handle id field in response - can be string, number, or null
    if (request.id.empty()) {
        response_json["id"] = nullptr;  // JSON null for empty/null id
    } else {
        // Try to parse as number first, fallback to string
        try {
            if (request.id.find_first_not_of("0123456789.-") == std::string::npos) {
                // Looks like a number
                response_json["id"] = std::stod(request.id);
            } else {
                // Treat as string
                response_json["id"] = request.id;
            }
        } catch (const std::exception&) {
            // Fallback to string
            response_json["id"] = request.id;
        }
    }

    if (!mcp_response.error.empty()) {
        response_json["error"] = crow::json::load(mcp_response.error);
    } else {
        response_json["result"] = crow::json::load(mcp_response.result);
    }

    auto response = crow::response(200, response_json.dump());
    response.set_header("Content-Type", "application/json");
    addSessionHeaderToResponse(response, session_id);
    return response;
}

crow::response MCPRouteHandlers::createJsonRpcErrorResponse(const std::string& id, int code, const std::string& message,
                                                             const std::optional<std::string>& session_id) const {
    crow::json::wvalue error_json;
    error_json["code"] = code;
    error_json["message"] = message;

    crow::json::wvalue response_json;
    response_json["jsonrpc"] = "2.0";

    // Handle id field in error response - can be string, number, or null
    if (id.empty()) {
        response_json["id"] = nullptr;  // JSON null for empty/null id
    } else {
        // Try to parse as number first, fallback to string
        try {
            if (id.find_first_not_of("0123456789.-") == std::string::npos) {
                response_json["id"] = std::stod(id);
            } else {
                response_json["id"] = id;
            }
        } catch (const std::exception&) {
            response_json["id"] = id;
        }
    }

    response_json["error"] = std::move(error_json);

    auto response = crow::response(400, response_json.dump());
    response.set_header("Content-Type", "application/json");
    addSessionHeaderToResponse(response, session_id);
    return response;
}

void MCPRouteHandlers::discoverMCPEntities() const {
    // Create non-const copy to call the implementation
    const_cast<MCPRouteHandlers*>(this)->discoverMCPEntitiesImpl();
}

std::vector<crow::json::wvalue> MCPRouteHandlers::getToolDefinitionsFromConfig() const {
    auto tools = getToolDefinitionsImpl();
    CROW_LOG_DEBUG << "getToolDefinitionsFromConfig returning " << tools.size() << " tools";
    return tools;
}

std::vector<crow::json::wvalue> MCPRouteHandlers::getResourceDefinitionsFromConfig() const {
    return getResourceDefinitionsImpl();
}

void MCPRouteHandlers::discoverMCPEntities() {
    CROW_LOG_INFO << "Starting MCP entity discovery...";
    std::lock_guard<std::mutex> lock(state_mutex_);

    tool_definitions_.clear();
    resource_definitions_.clear();

    const auto& endpoints = config_manager_->getEndpoints();
    CROW_LOG_DEBUG << "Found " << endpoints.size() << " total endpoints";

    for (const auto& endpoint : endpoints) {
        //CROW_LOG_DEBUG << "Checking endpoint: REST=" << endpoint.isRESTEndpoint()
        //               << ", MCPTool=" << endpoint.isMCPTool()
        //               << ", MCPResource=" << endpoint.isMCPResource();

        if (endpoint.isMCPTool()) {
            CROW_LOG_DEBUG << "Adding MCP tool: " << (endpoint.mcp_tool ? endpoint.mcp_tool->name : "null");
            tool_definitions_.push_back(endpointToMCPToolDefinition(endpoint));
        }
        if (endpoint.isMCPResource()) {
            CROW_LOG_DEBUG << "Adding MCP resource: " << (endpoint.mcp_resource ? endpoint.mcp_resource->name : "null");
            resource_definitions_.push_back(endpointToMCPResourceDefinition(endpoint));
        }
    }

    CROW_LOG_INFO << "Discovered " << tool_definitions_.size() << " MCP tools and "
                   << resource_definitions_.size() << " MCP resources";
}

std::vector<crow::json::wvalue> MCPRouteHandlers::getToolDefinitions() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return tool_definitions_;
}

std::vector<crow::json::wvalue> MCPRouteHandlers::getResourceDefinitions() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return resource_definitions_;
}

// Implementation methods for const access
void MCPRouteHandlers::discoverMCPEntitiesImpl() {
    CROW_LOG_DEBUG << "Starting MCP entity discovery...";
    std::lock_guard<std::mutex> lock(state_mutex_);

    tool_definitions_.clear();
    resource_definitions_.clear();

    const auto& endpoints = config_manager_->getEndpoints();
    CROW_LOG_INFO << "Found " << endpoints.size() << " total endpoints in config manager";

    if (endpoints.empty()) {
        CROW_LOG_WARNING << "No endpoints found in config manager - tool discovery will be empty";
        return;
    }

    for (const auto& endpoint : endpoints) {
        CROW_LOG_DEBUG << "Checking endpoint: REST=" << endpoint.isRESTEndpoint()
                       << ", MCPTool=" << endpoint.isMCPTool()
                       << ", MCPResource=" << endpoint.isMCPResource();

        if (endpoint.isMCPTool()) {
            CROW_LOG_INFO << "Adding MCP tool: " << (endpoint.mcp_tool ? endpoint.mcp_tool->name : "null");
            tool_definitions_.push_back(endpointToMCPToolDefinition(endpoint));
        }
        if (endpoint.isMCPResource()) {
            CROW_LOG_INFO << "Adding MCP resource: " << (endpoint.mcp_resource ? endpoint.mcp_resource->name : "null");
            resource_definitions_.push_back(endpointToMCPResourceDefinition(endpoint));
        }
    }

    CROW_LOG_INFO << "Discovered " << tool_definitions_.size() << " MCP tools and "
                   << resource_definitions_.size() << " MCP resources";
}

std::vector<crow::json::wvalue> MCPRouteHandlers::getToolDefinitionsImpl() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    CROW_LOG_DEBUG << "getToolDefinitionsImpl returning " << tool_definitions_.size() << " tools from member variable";
    return tool_definitions_;
}

std::vector<crow::json::wvalue> MCPRouteHandlers::getResourceDefinitionsImpl() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return resource_definitions_;
}

crow::json::wvalue MCPRouteHandlers::endpointToMCPToolDefinition(const EndpointConfig& endpoint) const {
    crow::json::wvalue tool_def;

    // Basic tool information
    tool_def["name"] = endpoint.mcp_tool->name;
    tool_def["description"] = endpoint.mcp_tool->description;

    // Build input schema from request fields
    tool_def["inputSchema"]["type"] = "object";
    tool_def["inputSchema"]["properties"] = crow::json::wvalue();

    std::vector<std::string> required_fields;
    for (const auto& field : endpoint.request_fields) {
        crow::json::wvalue prop;
        prop["type"] = "string"; // Default type, could be enhanced based on validators
        prop["description"] = field.description;

        if (field.required) {
            required_fields.push_back(field.fieldName);
        }

        tool_def["inputSchema"]["properties"][field.fieldName] = std::move(prop);
    }

    if (!required_fields.empty()) {
        tool_def["inputSchema"]["required"] = required_fields;
    }

    return tool_def;
}

crow::json::wvalue MCPRouteHandlers::endpointToMCPResourceDefinition(const EndpointConfig& endpoint) const {
    crow::json::wvalue resource_def;

    resource_def["name"] = endpoint.mcp_resource->name;
    resource_def["description"] = endpoint.mcp_resource->description;
    resource_def["mimeType"] = endpoint.mcp_resource->mime_type;
    resource_def["uri"] = "flapi://" + endpoint.mcp_resource->name;

    return resource_def;
}

// JSON-RPC message handling methods
MCPResponse MCPRouteHandlers::handleMessage(const MCPRequest& request, const crow::request& http_req) const {
    MCPResponse response;
    response.id = request.id;

    CROW_LOG_DEBUG << "handleMessage called with method: '" << request.method << "'";

    try {
        if (request.method == "initialize") {
            response = handleInitializeRequest(request, http_req);
        } else if (request.method == "tools/list") {
            response = handleToolsListRequest(request, http_req);
        } else if (request.method == "tools/call") {
            response = handleToolsCallRequest(request, http_req);
        } else if (request.method == "resources/list") {
            response = handleResourcesListRequest(request, http_req);
        } else if (request.method == "resources/read") {
            response = handleResourcesReadRequest(request, http_req);
        } else if (request.method == "prompts/list") {
            response = handlePromptsListRequest(request, http_req);
        } else if (request.method == "prompts/get") {
            response = handlePromptsGetRequest(request, http_req);
        } else if (request.method == "logging/setLevel") {
            response = handleLoggingSetLevelRequest(request, http_req);
        } else if (request.method == "completion/complete") {
            response = handleCompletionCompleteRequest(request, http_req);
        } else if (request.method == "ping") {
            response = handlePingRequest(request, http_req);
        } else {
            response.error = "{\"code\":-32601,\"message\":\"Method not found\"}";
        }
    } catch (const std::exception& e) {
        CROW_LOG_ERROR << "Error handling MCP method '" << request.method << "': " << e.what();
        response.error = "{\"code\":-32603,\"message\":\"Internal error: " + std::string(e.what()) + "\"}";
    }

    return response;
}

MCPResponse MCPRouteHandlers::handleInitializeRequest(const MCPRequest& request, const crow::request& http_req) const {
    MCPResponse response;
    response.id = request.id;

    // Layer 1: Protocol-level authentication (mcp.auth)
    std::optional<MCPSession::AuthContext> auth_context;
    if (auth_handler_ && auth_handler_->methodRequiresAuth("initialize")) {
        auth_context = auth_handler_->authenticate(http_req);
        if (!auth_context) {
            CROW_LOG_WARNING << "MCP initialize: authentication required but failed";
            response.error = "{\"code\":-32001,\"message\":\"Authentication required for initialize\"}";
            return response;
        }
        CROW_LOG_INFO << "MCP initialize: authenticated as " << auth_context->username;
    }

    try {
        // Extract client protocol version from request params
        std::string client_version = "2024-11-05";  // Default to oldest supported for compatibility
        if (request.params.count("protocolVersion") > 0) {
            auto version_value = request.params["protocolVersion"];
            if (version_value.t() == crow::json::type::String) {
                std::string version_str = version_value.dump();
                // Remove quotes if present
                if (version_str.length() >= 2 && version_str.front() == '"' && version_str.back() == '"') {
                    client_version = version_str.substr(1, version_str.length() - 2);
                } else {
                    client_version = version_str;
                }
            }
        }

        // Negotiate protocol version - select highest mutually supported version
        // Supported versions: 2024-11-05, 2025-03-26, 2025-06-18, 2025-11-25
        std::string negotiated_version = "2024-11-05";  // Minimum supported

        if (client_version == "2025-11-25" || client_version == "2025-06-18" ||
            client_version == "2025-03-26" || client_version == "2024-11-05") {
            // Client requested a supported version - use the minimum of client and server
            if (client_version == "2025-11-25") {
                negotiated_version = "2025-11-25";
            } else if (client_version == "2025-06-18") {
                negotiated_version = "2025-06-18";
            } else if (client_version == "2025-03-26") {
                negotiated_version = "2025-03-26";
            } else {
                negotiated_version = "2024-11-05";
            }
        } else {
            // Unknown version - try to handle gracefully by using latest server supports
            CROW_LOG_WARNING << "Client requested unknown protocol version: " << client_version
                           << ", using server default: " << server_info_.protocol_version;
            negotiated_version = server_info_.protocol_version;
        }

        CROW_LOG_DEBUG << "Protocol version negotiated - Client: " << client_version
                      << ", Server: " << server_info_.protocol_version
                      << ", Negotiated: " << negotiated_version;

        crow::json::wvalue result;
        result["protocolVersion"] = negotiated_version;
        result["capabilities"] = crow::json::wvalue();
        result["capabilities"]["tools"] = crow::json::wvalue();
        result["capabilities"]["tools"]["listChanged"] = true;
        result["capabilities"]["resources"] = crow::json::wvalue();
        result["capabilities"]["resources"]["subscribe"] = false;
        result["capabilities"]["resources"]["listChanged"] = true;
        result["capabilities"]["prompts"] = crow::json::wvalue();
        result["capabilities"]["prompts"]["listChanged"] = true;
        result["capabilities"]["logging"] = crow::json::wvalue();  // NEW in 2025-11-25
        result["serverInfo"]["name"] = server_info_.name;
        result["serverInfo"]["version"] = server_info_.version;

        // Add instructions if configured
        std::string instructions = config_manager_->loadMCPInstructions();
        if (!instructions.empty()) {
            result["instructions"] = instructions;
            CROW_LOG_DEBUG << "Included MCP instructions in initialize response ("
                           << instructions.length() << " characters)";
        }

        response.result = result.dump();
    } catch (const std::exception& e) {
        CROW_LOG_ERROR << "Initialize error: " << e.what();
        response.error = "{\"code\":-32603,\"message\":\"Initialize error: " + std::string(e.what()) + "\"}";
    }

    return response;
}

MCPResponse MCPRouteHandlers::handleToolsListRequest(const MCPRequest& request, const crow::request& http_req) const {
    auto response = initResponse(request);
    // NOTE: http_req is available here for authentication when needed

    try {
        auto tool_definitions = getToolDefinitionsFromConfig();
        CROW_LOG_DEBUG << "Tools list request: found " << tool_definitions.size() << " tools";

        crow::json::wvalue result;
        crow::json::wvalue tools_array = crow::json::wvalue::list();

        for (size_t i = 0; i < tool_definitions.size(); ++i) {
            tools_array[i] = crow::json::wvalue(tool_definitions[i]);
        }

        result["tools"] = std::move(tools_array);

        response.result = result.dump();
        CROW_LOG_DEBUG << "Tools list response: " << response.result;
    } catch (const std::exception& e) {
        CROW_LOG_ERROR << "Tools list error: " << e.what();
        response.error = formatJsonRpcError(-32603, "Tools list error: " + std::string(e.what()));
    }

    return response;
}

MCPResponse MCPRouteHandlers::handleToolsCallRequest(const MCPRequest& request, const crow::request& http_req) const {
    auto response = initResponse(request);
    // NOTE: http_req is available here for authentication when needed

    try {
        // Extract tool name from params
        std::string tool_name;
        if (!extractRequiredStringParam(request.params, "name", tool_name, response)) {
            return response;
        }

        // Extract arguments from params
        crow::json::wvalue arguments;
        if (request.params.count("arguments") > 0) {
            arguments = crow::json::wvalue(request.params["arguments"]);
        } else {
            arguments = crow::json::wvalue::object();
        }

        CROW_LOG_DEBUG << "Tool call request: " << tool_name;

        // Use MCPToolHandler if available
        if (tool_handler_) {
            MCPToolCallRequest tool_request;
            tool_request.tool_name = tool_name;
            tool_request.arguments = crow::json::wvalue(arguments);

            auto result = tool_handler_->executeTool(tool_request);

            if (result.success) {
                // Convert result to MCP format using ContentResponse
                mcp::ContentResponse content_response;
                content_response.addText(result.result);
                crow::json::wvalue mcp_result = content_response.toJson();
                response.result = mcp_result.dump();
            } else {
                response.error = formatJsonRpcError(-32603, "Tool execution failed: " + result.error_message);
            }
        } else {
            response.error = formatJsonRpcError(-32601, "Tool handler not available");
        }
    } catch (const std::exception& e) {
        CROW_LOG_ERROR << "Tool call error: " << e.what();
        response.error = formatJsonRpcError(-32603, "Tool call error: " + std::string(e.what()));
    }

    return response;
}

MCPResponse MCPRouteHandlers::handleResourcesListRequest(const MCPRequest& request, const crow::request& http_req) const {
    auto response = initResponse(request);
    // NOTE: http_req is available here for authentication when needed

    try {
        auto resource_definitions = getResourceDefinitionsFromConfig();

        crow::json::wvalue result;
        crow::json::wvalue resources_array = crow::json::wvalue::list();

        for (size_t i = 0; i < resource_definitions.size(); ++i) {
            resources_array[i] = crow::json::wvalue(resource_definitions[i]);
        }

        result["resources"] = std::move(resources_array);

        response.result = result.dump();
    } catch (const std::exception& e) {
        response.error = formatJsonRpcError(-32603, "Resources list error: " + std::string(e.what()));
    }

    return response;
}

MCPResponse MCPRouteHandlers::handleResourcesReadRequest(const MCPRequest& request, const crow::request& http_req) const {
    auto response = initResponse(request);
    // NOTE: http_req is available here for authentication when needed

    try {
        // Extract resource URI from params
        std::string resource_uri;
        if (!extractRequiredStringParam(request.params, "uri", resource_uri, response)) {
            return response;
        }
        CROW_LOG_DEBUG << "Resource read request: " << resource_uri;

        // Find the resource configuration by URI
        auto resource_config = findResourceByURI(resource_uri);
        if (!resource_config) {
            response.error = formatJsonRpcError(-32602, "Resource not found: " + resource_uri);
            return response;
        }

        CROW_LOG_DEBUG << "Reading resource: " << resource_config->mcp_resource->name;

        // Read the resource content
        try {
            crow::json::wvalue result = readResourceContent(*resource_config);
            response.result = result.dump();
        } catch (const std::exception& e) {
            response.error = formatJsonRpcError(-32603, "Resource read error: " + std::string(e.what()));
        }
    } catch (const std::exception& e) {
        response.error = formatJsonRpcError(-32603, "Resource read error: " + std::string(e.what()));
    }

    return response;
}

// Resource functionality implementation
std::optional<EndpointConfig> MCPRouteHandlers::findResourceByURI(const std::string& uri) const {
    // For now, use a simple URI scheme: flapi://resource_name
    // TODO: Make this more sophisticated with proper URI parsing
    if (uri.find("flapi://") != 0) {
        return std::nullopt;
    }

    std::string resource_name = uri.substr(8); // Remove "flapi://" prefix

    // Find the resource configuration by name
    const auto& endpoints = config_manager_->getEndpoints();
    for (const auto& endpoint : endpoints) {
        if (endpoint.isMCPResource() && endpoint.mcp_resource->name == resource_name) {
            return endpoint;
        }
    }

    return std::nullopt;
}

crow::json::wvalue MCPRouteHandlers::readResourceContent(const EndpointConfig& resource_config) const {
    crow::json::wvalue result;

    // Execute the resource's template to get the content
    try {
        // For MCP resources, we need to execute the SQL template using the database manager
        if (db_manager_) {
            // Prepare empty parameters for resource reading (resources don't take input parameters)
            std::map<std::string, std::string> params;

            // Execute the query using the same method as tools
            auto query_result = db_manager_->executeQuery(resource_config, params, false);

            // Check if the query result structure has the data we need
            if (query_result.data.size() > 0) {
                // Get the result as a JSON string
                std::string result_text;
                if (resource_config.mcp_resource->mime_type == "application/json" ||
                    resource_config.mcp_resource->mime_type.find("json") != std::string::npos) {
                    result_text = query_result.data.dump();
                } else {
                    // For text/plain, convert JSON to a readable text format
                    result_text = query_result.data.dump(); // For now, use JSON format
                }

                // Convert the result to MCP resource content format
                crow::json::wvalue content_item;
                content_item["uri"] = "flapi://" + resource_config.mcp_resource->name;
                content_item["mimeType"] = resource_config.mcp_resource->mime_type;
                content_item["text"] = result_text;

                crow::json::wvalue contents_array = crow::json::wvalue::list();
                contents_array[0] = std::move(content_item);

                result["contents"] = std::move(contents_array);
            } else {
                throw std::runtime_error("Resource query returned no data");
            }
        } else {
            // Fallback: return a simple resource representation
            crow::json::wvalue content_item;
            content_item["uri"] = "flapi://" + resource_config.mcp_resource->name;
            content_item["mimeType"] = resource_config.mcp_resource->mime_type;
            content_item["text"] = "Resource content for: " + resource_config.mcp_resource->name + " (database not available)";

            crow::json::wvalue contents_array = crow::json::wvalue::list();
            contents_array[0] = std::move(content_item);

            result["contents"] = std::move(contents_array);
        }
    } catch (const std::exception& e) {
        CROW_LOG_ERROR << "Error reading resource " << resource_config.mcp_resource->name << ": " << e.what();
        throw;
    }

    return result;
}

// Prompt functionality implementation
MCPResponse MCPRouteHandlers::handlePromptsListRequest(const MCPRequest& request, const crow::request& http_req) const {
    auto response = initResponse(request);
    // NOTE: http_req is available here for authentication when needed

    try {
        crow::json::wvalue result;
        crow::json::wvalue prompts_array = crow::json::wvalue::list();

        // Find all prompt endpoints
        const auto& endpoints = config_manager_->getEndpoints();
        int prompt_index = 0;

        for (const auto& endpoint : endpoints) {
            if (endpoint.isMCPPrompt()) {
                crow::json::wvalue prompt_def = endpointToMCPPromptDefinition(endpoint);
                prompts_array[prompt_index++] = std::move(prompt_def);
            }
        }

        result["prompts"] = std::move(prompts_array);
        response.result = result.dump();
    } catch (const std::exception& e) {
        response.error = formatJsonRpcError(-32603, "Prompts list error: " + std::string(e.what()));
    }

    return response;
}

MCPResponse MCPRouteHandlers::handlePromptsGetRequest(const MCPRequest& request, const crow::request& http_req) const {
    auto response = initResponse(request);
    // NOTE: http_req is available here for authentication when needed

    try {
        // Extract prompt name from parameters
        std::string prompt_name;
        if (!extractRequiredStringParam(request.params, "name", prompt_name, response)) {
            return response;
        }

        CROW_LOG_DEBUG << "Prompt get request: " << prompt_name;

        // Find the prompt configuration by name
        auto prompt_config = findPromptByName(prompt_name);
        if (!prompt_config) {
            response.error = formatJsonRpcError(-32602, "Prompt not found: " + prompt_name);
            return response;
        }

        // Get arguments for template processing
        const crow::json::wvalue* arguments_ptr = nullptr;
        if (request.params.count("arguments") && request.params["arguments"].t() != crow::json::type::Null) {
            arguments_ptr = &request.params["arguments"];
        }

        // Process the prompt template
        crow::json::wvalue result = processPromptTemplate(*prompt_config, arguments_ptr);
        response.result = result.dump();
    } catch (const std::exception& e) {
        response.error = formatJsonRpcError(-32603, "Prompt get error: " + std::string(e.what()));
    }

    return response;
}

std::optional<EndpointConfig> MCPRouteHandlers::findPromptByName(const std::string& name) const {
    const auto& endpoints = config_manager_->getEndpoints();
    for (const auto& endpoint : endpoints) {
        if (endpoint.isMCPPrompt() && endpoint.mcp_prompt->name == name) {
            return endpoint;
        }
    }
    return std::nullopt;
}

crow::json::wvalue MCPRouteHandlers::processPromptTemplate(const EndpointConfig& prompt_config, const crow::json::wvalue* arguments) const {
    crow::json::wvalue result;

    try {
        std::string template_content = prompt_config.mcp_prompt->template_content;

        // Simple parameter substitution - replace {{param_name}} with argument values
        for (const auto& arg_name : prompt_config.mcp_prompt->arguments) {
            std::string placeholder = "{{" + arg_name + "}}";
            std::string replacement = "";

            if (arguments && arguments->count(arg_name) && (*arguments)[arg_name].t() != crow::json::type::Null) {
                const auto& arg_value = (*arguments)[arg_name];
                if (arg_value.t() == crow::json::type::String) {
                    std::string arg_str = arg_value.dump();
                    if (arg_str.length() >= 2 && arg_str.front() == '"' && arg_str.back() == '"') {
                        replacement = arg_str.substr(1, arg_str.length() - 2);
                    } else {
                        replacement = arg_str;
                    }
                } else {
                    replacement = arg_value.dump();
                }
            }

            size_t pos = 0;
            while ((pos = template_content.find(placeholder, pos)) != std::string::npos) {
                template_content.replace(pos, placeholder.length(), replacement);
                pos += replacement.length();
            }
        }

        // Create the prompt response
        result["description"] = prompt_config.mcp_prompt->description;
        result["messages"] = crow::json::wvalue::list();

        // Create a user message with the processed template
        crow::json::wvalue message;
        message["role"] = "user";
        message["content"]["type"] = "text";
        message["content"]["text"] = template_content;

        result["messages"][0] = std::move(message);

    } catch (const std::exception& e) {
        CROW_LOG_ERROR << "Error processing prompt template " << prompt_config.mcp_prompt->name << ": " << e.what();
        throw;
    }

    return result;
}

crow::json::wvalue MCPRouteHandlers::endpointToMCPPromptDefinition(const EndpointConfig& endpoint) const {
    crow::json::wvalue prompt_def;

    prompt_def["name"] = endpoint.mcp_prompt->name;
    prompt_def["description"] = endpoint.mcp_prompt->description;

    // Add arguments as JSON schema
    prompt_def["arguments"] = crow::json::wvalue::list();
    for (size_t i = 0; i < endpoint.mcp_prompt->arguments.size(); ++i) {
        crow::json::wvalue arg_def;
        arg_def["name"] = endpoint.mcp_prompt->arguments[i];
        arg_def["type"] = "string";  // Default to string for now
        arg_def["description"] = "Parameter " + endpoint.mcp_prompt->arguments[i];
        prompt_def["arguments"][i] = std::move(arg_def);
    }

    return prompt_def;
}

// Ping functionality implementation
MCPResponse MCPRouteHandlers::handlePingRequest(const MCPRequest& request, const crow::request& http_req) const {
    MCPResponse response;
    // NOTE: http_req is available here for authentication when needed
    response.id = request.id;

    try {
        CROW_LOG_DEBUG << "Ping request received";

        // MCP ping response should be an empty object per specification
        // This complies with standard JSON-RPC ping implementations
        crow::json::wvalue result = crow::json::wvalue::object();
        // Empty object - no additional fields needed for ping

        response.result = result.dump();
    } catch (const std::exception& e) {
        response.error = "{\"code\":-32603,\"message\":\"Ping error: " + std::string(e.what()) + "\"}";
    }

    return response;
}

MCPResponse MCPRouteHandlers::handleLoggingSetLevelRequest(const MCPRequest& request, const crow::request& http_req) const {
    MCPResponse response;
    // NOTE: http_req is available here for authentication when needed
    response.id = request.id;

    try {
        // Extract log level from params
        if (request.params.count("level") == 0) {
            response.error = "{\"code\":-32602,\"message\":\"Invalid params: missing 'level' field\"}";
            return response;
        }

        auto level_value = request.params["level"];
        std::string log_level;

        if (JsonUtils::isString(level_value)) {
            log_level = JsonUtils::extractString(level_value);
        } else {
            response.error = "{\"code\":-32602,\"message\":\"Invalid params: 'level' must be a string\"}";
            return response;
        }

        // Map MCP log levels to Crow log levels
        // MCP levels: debug, info, notice, warning, error, critical, alert, emergency
        // Crow levels: DEBUG, INFO, WARNING, ERROR
        crow::LogLevel crow_level = crow::LogLevel::INFO;

        if (log_level == "debug") {
            crow_level = crow::LogLevel::DEBUG;
        } else if (log_level == "info" || log_level == "notice") {
            crow_level = crow::LogLevel::INFO;
        } else if (log_level == "warning") {
            crow_level = crow::LogLevel::WARNING;
        } else if (log_level == "error" || log_level == "critical" || log_level == "alert" || log_level == "emergency") {
            crow_level = crow::LogLevel::ERROR;
        } else {
            response.error = "{\"code\":-32602,\"message\":\"Invalid log level: " + log_level + "\"}";
            return response;
        }

        // Set the log level globally
        CROW_LOG_INFO << "Setting log level to: " << log_level;
        crow::logger::setLogLevel(crow_level);

        // Return success response (empty object)
        crow::json::wvalue result = crow::json::wvalue::object();
        response.result = result.dump();

    } catch (const std::exception& e) {
        CROW_LOG_ERROR << "logging/setLevel error: " << e.what();
        response.error = "{\"code\":-32603,\"message\":\"Internal error: " + std::string(e.what()) + "\"}";
    }

    return response;
}

MCPResponse MCPRouteHandlers::handleCompletionCompleteRequest(const MCPRequest& request, const crow::request& http_req) const {
    MCPResponse response;
    // NOTE: http_req is available here for authentication when needed
    response.id = request.id;

    try {
        // Extract parameters from request
        // ref: reference to tool/prompt/resource (e.g., "customer_lookup" or "prompt_name")
        // argument: argument name to complete
        // value: partial value prefix

        if (request.params.count("ref") == 0 || request.params.count("argument") == 0) {
            response.error = "{\"code\":-32602,\"message\":\"Invalid params: missing 'ref' or 'argument' field\"}";
            return response;
        }

        // Extract ref (reference to tool/prompt)
        auto ref_value = request.params["ref"];
        std::string ref_str;
        if (JsonUtils::isString(ref_value)) {
            ref_str = JsonUtils::extractString(ref_value);
        } else {
            response.error = "{\"code\":-32602,\"message\":\"Invalid params: 'ref' must be a string\"}";
            return response;
        }

        // Extract argument name
        auto arg_value = request.params["argument"];
        std::string argument_name;
        if (JsonUtils::isString(arg_value)) {
            argument_name = JsonUtils::extractString(arg_value);
        } else {
            response.error = "{\"code\":-32602,\"message\":\"Invalid params: 'argument' must be a string\"}";
            return response;
        }

        // Extract optional value (prefix) for filtering
        std::string value_prefix;
        if (request.params.count("value") > 0) {
            auto value_val = request.params["value"];
            if (JsonUtils::isString(value_val)) {
                value_prefix = JsonUtils::extractString(value_val);
            }
        }

        CROW_LOG_DEBUG << "Completion request: ref=" << ref_str << ", argument=" << argument_name
                      << ", prefix=" << value_prefix;

        // Find the tool/prompt by reference name
        std::optional<EndpointConfig> endpoint;
        auto endpoints = config_manager_->getEndpoints();
        for (const auto& ep : endpoints) {
            if ((ep.isMCPTool() && ep.mcp_tool && ep.mcp_tool->name == ref_str) ||
                (ep.isMCPPrompt() && ep.mcp_prompt && ep.mcp_prompt->name == ref_str)) {
                endpoint = ep;
                break;
            }
        }

        if (!endpoint) {
            response.error = "{\"code\":-32602,\"message\":\"Reference not found: " + ref_str + "\"}";
            return response;
        }

        // Find the argument field by name
        const RequestFieldConfig* matching_field = nullptr;
        for (const auto& field : endpoint->request_fields) {
            if (field.fieldName == argument_name) {
                matching_field = &field;
                break;
            }
        }

        if (!matching_field) {
            response.error = "{\"code\":-32602,\"message\":\"Argument not found: " + argument_name + "\"}";
            return response;
        }

        // Build completion suggestions based on validators
        crow::json::wvalue completion;
        crow::json::wvalue values = crow::json::wvalue::list();
        int total_count = 0;
        bool has_more = false;

        // Check for enum validator to provide enum values
        for (const auto& validator : matching_field->validators) {
            if (validator.type == "enum" && validator.allowedValues.size() > 0) {
                // Filter enum values based on prefix
                int added_count = 0;
                for (const auto& enum_val : validator.allowedValues) {
                    if (value_prefix.empty() || enum_val.find(value_prefix) == 0) {
                        if (added_count < 50) {  // Limit results to 50
                            values[added_count] = enum_val;
                            added_count++;
                        } else {
                            has_more = true;
                            break;
                        }
                    }
                    total_count++;
                }
                break;
            }
        }

        // If no enum validator, return empty completion (client can use its own methods)
        completion["values"] = std::move(values);
        completion["total"] = total_count;
        completion["hasMore"] = has_more;

        response.result = completion.dump();
        CROW_LOG_DEBUG << "Completion result: " << response.result;

    } catch (const std::exception& e) {
        CROW_LOG_ERROR << "completion/complete error: " << e.what();
        response.error = "{\"code\":-32603,\"message\":\"Internal error: " + std::string(e.what()) + "\"}";
    }

    return response;
}

} // namespace flapi

