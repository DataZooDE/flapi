#include "mcp_server.hpp"
#include <iostream>
#include <sstream>

namespace flapi {

MCPServer::MCPServer(std::shared_ptr<ConfigManager> config_manager,
                    std::shared_ptr<DatabaseManager> db_manager)
    : config_manager(config_manager), db_manager(db_manager)
{
    // Initialize server info with default values (no separate MCP config needed)
    server_info.name = "flapi-mcp-server";
    server_info.version = "0.3.0";
    server_info.protocol_version = "2024-11-05";

    // Initialize server capabilities with all available capabilities
    server_capabilities.tools = {"tools", "resources"};
    // Note: prompts and sampling capabilities would be added based on available endpoints

    // Discover MCP entities from unified configuration
    discoverMCPEntities();

    CROW_LOG_WARNING << "MCPServer is deprecated. Use MCPRouteHandlers instead for new implementations.";
    CROW_LOG_INFO << "MCP Server initialized with " << tool_definitions.size() << " tools and "
                  << resource_definitions.size() << " resources";
}

MCPServer::~MCPServer() {
    // No server to stop in deprecated implementation
}

// Deprecated server methods removed - use MCPRouteHandlers instead
void MCPServer::run(int port) {
    CROW_LOG_ERROR << "MCPServer::run() is deprecated. MCP functionality is now handled by APIServer with MCPRouteHandlers.";
    // No-op for backward compatibility
}

void MCPServer::stop() {
    CROW_LOG_DEBUG << "MCPServer::stop() called - no server to stop in deprecated implementation";
    // No-op for backward compatibility
}

// Deprecated HTTP handler methods removed - use MCPRouteHandlers instead

void MCPServer::discoverMCPEntities() {
    std::lock_guard<std::mutex> lock(state_mutex);

    tool_definitions.clear();
    resource_definitions.clear();

    const auto& endpoints = config_manager->getEndpoints();

    for (const auto& endpoint : endpoints) {
        if (endpoint.isMCPTool()) {
            tool_definitions.push_back(endpointToMCPToolDefinition(endpoint));
        }
        if (endpoint.isMCPResource()) {
            resource_definitions.push_back(endpointToMCPResourceDefinition(endpoint));
        }
    }

    CROW_LOG_DEBUG << "Discovered " << tool_definitions.size() << " MCP tools and "
                   << resource_definitions.size() << " MCP resources";
}

crow::json::wvalue MCPServer::endpointToMCPToolDefinition(const EndpointConfig& endpoint) const {
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

crow::json::wvalue MCPServer::endpointToMCPResourceDefinition(const EndpointConfig& endpoint) const {
    crow::json::wvalue resource_def;

    resource_def["name"] = endpoint.mcp_resource->name;
    resource_def["description"] = endpoint.mcp_resource->description;
    resource_def["mimeType"] = endpoint.mcp_resource->mime_type;

    return resource_def;
}

std::vector<crow::json::wvalue> MCPServer::getToolDefinitions() const {
    std::lock_guard<std::mutex> lock(state_mutex);
    return tool_definitions;
}


} // namespace flapi
