#pragma once

#include <crow.h>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <queue>
#include <mutex>
#include <atomic>

#include "config_manager.hpp"
#include "database_manager.hpp"
#include "mcp_tool_handler.hpp"
#include "mcp_types.hpp"

namespace flapi {

/**
 * @deprecated MCPServer is deprecated in favor of unified architecture.
 * Use MCPRouteHandlers for MCP functionality instead.
 *
 * This class is kept for backward compatibility but should not be used in new code.
 */
class [[deprecated("Use MCPRouteHandlers instead")]] MCPServer {
public:
    explicit MCPServer(std::shared_ptr<ConfigManager> config_manager,
                      std::shared_ptr<DatabaseManager> db_manager);
    ~MCPServer();

    void run(int port = 8081);
    void stop();

    // MCP protocol methods
    MCPServerInfo getServerInfo() const;
    MCPServerCapabilities getServerCapabilities() const;

    // Tool and resource discovery from unified configuration
    std::vector<crow::json::wvalue> getToolDefinitions() const;
    std::vector<crow::json::wvalue> getResourceDefinitions() const;

private:
    // Tool and resource discovery from unified configuration
    void discoverMCPEntities();
    crow::json::wvalue endpointToMCPToolDefinition(const EndpointConfig& endpoint) const;
    crow::json::wvalue endpointToMCPResourceDefinition(const EndpointConfig& endpoint) const;

    std::shared_ptr<ConfigManager> config_manager;
    std::shared_ptr<DatabaseManager> db_manager;

    // Server state
    MCPServerInfo server_info;
    MCPServerCapabilities server_capabilities;
    std::vector<crow::json::wvalue> tool_definitions;
    std::vector<crow::json::wvalue> resource_definitions;

    mutable std::mutex state_mutex;
};

} // namespace flapi
