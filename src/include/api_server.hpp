#pragma once

#define CROW_ENABLE_COMPRESSION
#include <crow.h>
#include "crow/middlewares/cors.h"
#include "crow/compression.h"

#include "auth_middleware.hpp"
#include "config_manager.hpp"
#include "database_manager.hpp"
#include "heartbeat_worker.hpp"
#include "open_api_doc_generator.hpp"
#include "rate_limit_middleware.hpp"
#include "request_handler.hpp"
#include "route_translator.hpp"
#include "mcp_route_handlers.hpp"
#include "mcp_session_manager.hpp"
#include "mcp_client_capabilities.hpp"

namespace flapi {

using FlapiApp = crow::App<crow::CORSHandler, RateLimitMiddleware, AuthMiddleware>;

class ConfigService;   // forward declaration
class HeartbeatWorker; // forward declaration

class APIServer 
{
public:
    explicit APIServer(std::shared_ptr<ConfigManager> config_manager, std::shared_ptr<DatabaseManager> db_manager, bool enable_config_ui);
    ~APIServer();

    crow::response getConfig();
    crow::response refreshConfig();
    
    void run(int port = 8080);
    void stop();

    void requestForEndpoint(const EndpointConfig& endpoint, const std::unordered_map<std::string, std::string>& pathParams = {});
    
    // Getters for heartbeat worker
    std::shared_ptr<CacheManager> getCacheManager() const;
    std::shared_ptr<DatabaseManager> getDatabaseManager() const;

private:
    void createApp();
    void setupRoutes();
    void setupCORS();
    void setupHeartbeat();
    void handleDynamicRequest(const crow::request& req, crow::response& res);
    crow::response generateOpenAPIDoc();
    
    FlapiApp app;
    std::shared_ptr<ConfigManager> configManager;
    std::shared_ptr<ConfigService> configService;
    std::shared_ptr<DatabaseManager> dbManager;
    std::shared_ptr<OpenAPIDocGenerator> openAPIDocGenerator;
    std::shared_ptr<HeartbeatWorker> heartbeatWorker;
    std::unique_ptr<MCPRouteHandlers> mcpRouteHandlers;
    std::shared_ptr<MCPSessionManager> mcpSessionManager;
    std::shared_ptr<MCPClientCapabilitiesDetector> mcpCapabilitiesDetector;
    RequestHandler requestHandler;
};

} // namespace flapi