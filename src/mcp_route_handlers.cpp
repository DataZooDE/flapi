#include "mcp_route_handlers.hpp"
#include <iostream>
#include <sstream>
#include <optional>

namespace flapi {

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

    // Initialize server info with default values (no separate MCP config needed)
    server_info_.name = "flapi-mcp-server";
    server_info_.version = "0.3.0";
    server_info_.protocol_version = "2024-11-05";

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

                // Parse and validate the request
                auto mcp_request = parseMCPRequest(req);
                if (!mcp_request) {
                    CROW_LOG_ERROR << "Failed to parse MCP request";
                    return createJsonRpcErrorResponse("", -32700, "Parse error: Invalid JSON");
                }

                CROW_LOG_DEBUG << "MCP request: method=" << mcp_request->method << ", id=" << mcp_request->id;

                // Dispatch the request to appropriate handler
                MCPResponse mcp_response = dispatchMCPRequest(*mcp_request);

                // Create and return JSON-RPC response
                return createJsonRpcResponse(*mcp_request, mcp_response);
            } catch (const std::exception& e) {
                CROW_LOG_ERROR << "Error handling MCP request: " << e.what();
                return createJsonRpcErrorResponse("", -32603, "Internal JSON-RPC error: " + std::string(e.what()));
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
    if (jsonrpc_value.t() == crow::json::type::String) {
        std::string jsonrpc_str = jsonrpc_value.dump();
        // Remove quotes if present
        if (jsonrpc_str.length() >= 2 && jsonrpc_str.front() == '"' && jsonrpc_str.back() == '"') {
            mcp_req.jsonrpc = jsonrpc_str.substr(1, jsonrpc_str.length() - 2);
        } else {
            mcp_req.jsonrpc = jsonrpc_str;
        }
    } else if (jsonrpc_value.t() == crow::json::type::Null) {
        mcp_req.jsonrpc = "2.0";  // Default to 2.0 for null
    } else {
        // Convert to string using dump()
        mcp_req.jsonrpc = jsonrpc_value.dump();
    }

    // Handle method field which should be a string
    auto method_value = json_request["method"];
    if (method_value.t() == crow::json::type::String) {
        std::string method_str = method_value.dump();
        // Remove quotes if present
        if (method_str.length() >= 2 && method_str.front() == '"' && method_str.back() == '"') {
            mcp_req.method = method_str.substr(1, method_str.length() - 2);
        } else {
            mcp_req.method = method_str;
        }
    } else if (method_value.t() == crow::json::type::Null) {
        mcp_req.method = "";  // Will be caught by validation
    } else {
        // Convert to string using dump()
        mcp_req.method = method_value.dump();
    }

    // Handle id field which can be string, number, or null
    auto id_value = json_request["id"];
    if (id_value.t() == crow::json::type::String) {
        // Extract string value without quotes
        std::string id_str = id_value.dump();
        if (id_str.length() >= 2 && id_str.front() == '"' && id_str.back() == '"') {
            mcp_req.id = id_str.substr(1, id_str.length() - 2);
        } else {
            mcp_req.id = id_str;
        }
    } else if (id_value.t() == crow::json::type::Number) {
        // Convert number to string using dump()
        mcp_req.id = id_value.dump();
    } else if (id_value.t() == crow::json::type::Null) {
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

MCPResponse MCPRouteHandlers::dispatchMCPRequest(const MCPRequest& request) const {
    return handleMessage(request);
}

crow::response MCPRouteHandlers::createJsonRpcResponse(const MCPRequest& request, const MCPResponse& mcp_response) const {
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
    return response;
}

crow::response MCPRouteHandlers::createJsonRpcErrorResponse(const std::string& id, int code, const std::string& message) const {
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
    CROW_LOG_DEBUG << "Starting MCP entity discovery...";
    std::lock_guard<std::mutex> lock(state_mutex_);

    tool_definitions_.clear();
    resource_definitions_.clear();

    const auto& endpoints = config_manager_->getEndpoints();
    CROW_LOG_DEBUG << "Found " << endpoints.size() << " total endpoints";

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

    CROW_LOG_DEBUG << "Discovered " << tool_definitions_.size() << " MCP tools and "
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
MCPResponse MCPRouteHandlers::handleMessage(const MCPRequest& request) const {
    MCPResponse response;
    response.id = request.id;

    CROW_LOG_DEBUG << "handleMessage called with method: '" << request.method << "'";

    try {
        if (request.method == "initialize") {
            response = handleInitializeRequest(request);
        } else if (request.method == "tools/list") {
            response = handleToolsListRequest(request);
        } else if (request.method == "tools/call") {
            response = handleToolsCallRequest(request);
        } else if (request.method == "resources/list") {
            response = handleResourcesListRequest(request);
        } else if (request.method == "resources/read") {
            response = handleResourcesReadRequest(request);
        } else if (request.method == "prompts/list") {
            response = handlePromptsListRequest(request);
        } else if (request.method == "prompts/get") {
            response = handlePromptsGetRequest(request);
        } else if (request.method == "ping") {
            response = handlePingRequest(request);
        } else {
            response.error = "{\"code\":-32601,\"message\":\"Method not found\"}";
        }
    } catch (const std::exception& e) {
        CROW_LOG_ERROR << "Error handling MCP method '" << request.method << "': " << e.what();
        response.error = "{\"code\":-32603,\"message\":\"Internal error: " + std::string(e.what()) + "\"}";
    }

    return response;
}

MCPResponse MCPRouteHandlers::handleInitializeRequest(const MCPRequest& request) const {
    MCPResponse response;
    response.id = request.id;

    try {
        crow::json::wvalue result;
        result["protocolVersion"] = server_info_.protocol_version;
        result["capabilities"] = crow::json::wvalue();
        result["capabilities"]["tools"] = crow::json::wvalue();
        result["capabilities"]["tools"]["listChanged"] = true;
        result["capabilities"]["resources"] = crow::json::wvalue();
        result["capabilities"]["resources"]["subscribe"] = false;
        result["capabilities"]["resources"]["listChanged"] = true;
        result["capabilities"]["prompts"] = crow::json::wvalue();
        result["capabilities"]["prompts"]["listChanged"] = true;
        result["serverInfo"]["name"] = server_info_.name;
        result["serverInfo"]["version"] = server_info_.version;

        response.result = result.dump();
    } catch (const std::exception& e) {
        response.error = "{\"code\":-32603,\"message\":\"Initialize error: " + std::string(e.what()) + "\"}";
    }

    return response;
}

MCPResponse MCPRouteHandlers::handleToolsListRequest(const MCPRequest& request) const {
    MCPResponse response;
    response.id = request.id;

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
        response.error = "{\"code\":-32603,\"message\":\"Tools list error: " + std::string(e.what()) + "\"}";
    }

    return response;
}

MCPResponse MCPRouteHandlers::handleToolsCallRequest(const MCPRequest& request) const {
    MCPResponse response;
    response.id = request.id;

    try {
        // Extract tool name from params
        if (request.params.count("name") == 0) {
            response.error = "{\"code\":-32602,\"message\":\"Invalid params: missing tool name\"}";
            return response;
        }

        // Handle name field which should be a string
        auto name_value = request.params["name"];
        std::string tool_name;

        if (name_value.t() == crow::json::type::Null) {
            response.error = "{\"code\":-32602,\"message\":\"Invalid params: tool name cannot be null\"}";
            return response;
        } else {
            // Extract string value properly
            if (name_value.t() == crow::json::type::String) {
                std::string name_str = name_value.dump();
                // Remove quotes if present
                if (name_str.length() >= 2 && name_str.front() == '"' && name_str.back() == '"') {
                    tool_name = name_str.substr(1, name_str.length() - 2);
                } else {
                    tool_name = name_str;
                }
            } else {
                // Convert other types using dump()
                tool_name = name_value.dump();
            }
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
                // Convert result to MCP format
                crow::json::wvalue mcp_result;
                crow::json::wvalue content_array = crow::json::wvalue::list();

                // Add the result content as text
                crow::json::wvalue content_item;
                content_item["type"] = "text";
                content_item["text"] = result.result;
                content_array[0] = crow::json::wvalue(content_item);

                mcp_result["content"] = std::move(content_array);

                response.result = mcp_result.dump();
            } else {
                response.error = "{\"code\":-32603,\"message\":\"Tool execution failed: " + result.error_message + "\"}";
            }
        } else {
            response.error = "{\"code\":-32601,\"message\":\"Tool handler not available\"}";
        }
    } catch (const std::exception& e) {
        CROW_LOG_ERROR << "Tool call error: " << e.what();
        response.error = "{\"code\":-32603,\"message\":\"Tool call error: " + std::string(e.what()) + "\"}";
    }

    return response;
}

MCPResponse MCPRouteHandlers::handleResourcesListRequest(const MCPRequest& request) const {
    MCPResponse response;
    response.id = request.id;

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
        response.error = "{\"code\":-32603,\"message\":\"Resources list error: " + std::string(e.what()) + "\"}";
    }

    return response;
}

MCPResponse MCPRouteHandlers::handleResourcesReadRequest(const MCPRequest& request) const {
    MCPResponse response;
    response.id = request.id;

    try {
        // Extract resource URI from params
        if (request.params.count("uri") == 0) {
            response.error = "{\"code\":-32602,\"message\":\"Invalid params: missing resource URI\"}";
            return response;
        }

        // Handle uri field which should be a string
        auto uri_value = request.params["uri"];
        std::string resource_uri;

        if (uri_value.t() == crow::json::type::Null) {
            response.error = "{\"code\":-32602,\"message\":\"Invalid params: resource URI cannot be null\"}";
            return response;
        } else {
            // Extract string value properly
            if (uri_value.t() == crow::json::type::String) {
                std::string uri_str = uri_value.dump();
                // Remove quotes if present
                if (uri_str.length() >= 2 && uri_str.front() == '"' && uri_str.back() == '"') {
                    resource_uri = uri_str.substr(1, uri_str.length() - 2);
                } else {
                    resource_uri = uri_str;
                }
            } else {
                // Convert other types using dump()
                resource_uri = uri_value.dump();
            }
        }
        CROW_LOG_DEBUG << "Resource read request: " << resource_uri;

        // Find the resource configuration by URI
        auto resource_config = findResourceByURI(resource_uri);
        if (!resource_config) {
            response.error = "{\"code\":-32602,\"message\":\"Resource not found: " + resource_uri + "\"}";
            return response;
        }

        CROW_LOG_DEBUG << "Reading resource: " << resource_config->mcp_resource->name;

        // Read the resource content
        try {
            crow::json::wvalue result = readResourceContent(*resource_config);
            response.result = result.dump();
        } catch (const std::exception& e) {
            response.error = "{\"code\":-32603,\"message\":\"Resource read error: " + std::string(e.what()) + "\"}";
        }
    } catch (const std::exception& e) {
        response.error = "{\"code\":-32603,\"message\":\"Resource read error: " + std::string(e.what()) + "\"}";
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
MCPResponse MCPRouteHandlers::handlePromptsListRequest(const MCPRequest& request) const {
    MCPResponse response;
    response.id = request.id;

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
        response.error = "{\"code\":-32603,\"message\":\"Prompts list error: " + std::string(e.what()) + "\"}";
    }

    return response;
}

MCPResponse MCPRouteHandlers::handlePromptsGetRequest(const MCPRequest& request) const {
    MCPResponse response;
    response.id = request.id;

    try {
        // Extract prompt name from parameters
        if (!request.params.count("name") || request.params["name"].t() == crow::json::type::Null) {
            response.error = "{\"code\":-32602,\"message\":\"Invalid params: prompt name is required\"}";
            return response;
        }

        std::string prompt_name;
        if (request.params["name"].t() == crow::json::type::String) {
            std::string name_str = request.params["name"].dump();
            if (name_str.length() >= 2 && name_str.front() == '"' && name_str.back() == '"') {
                prompt_name = name_str.substr(1, name_str.length() - 2);
            } else {
                prompt_name = name_str;
            }
        } else {
            prompt_name = request.params["name"].dump();
        }

        CROW_LOG_DEBUG << "Prompt get request: " << prompt_name;

        // Find the prompt configuration by name
        auto prompt_config = findPromptByName(prompt_name);
        if (!prompt_config) {
            response.error = "{\"code\":-32602,\"message\":\"Prompt not found: " + prompt_name + "\"}";
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
        response.error = "{\"code\":-32603,\"message\":\"Prompt get error: " + std::string(e.what()) + "\"}";
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
MCPResponse MCPRouteHandlers::handlePingRequest(const MCPRequest& request) const {
    MCPResponse response;
    response.id = request.id;

    try {
        CROW_LOG_DEBUG << "Ping request received";

        // Create a simple ping response
        crow::json::wvalue result;
        result["message"] = "pong";
        result["timestamp"] = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        result["server"] = server_info_.name;
        result["version"] = server_info_.version;

        response.result = result.dump();
    } catch (const std::exception& e) {
        response.error = "{\"code\":-32603,\"message\":\"Ping error: " + std::string(e.what()) + "\"}";
    }

    return response;
}

} // namespace flapi

