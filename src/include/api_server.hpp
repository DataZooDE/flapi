#pragma once

#include <crow.h>
#include "crow/middlewares/cors.h"

#include "auth_middleware.hpp"
#include "config_manager.hpp"
#include "database_manager.hpp"
#include "heartbeat_worker.hpp"
#include "open_api_doc_generator.hpp"
#include "rate_limit_middleware.hpp"
#include "request_handler.hpp"
#include "route_translator.hpp"

namespace flapi {

using FlapiApp = crow::App<crow::CORSHandler, RateLimitMiddleware, AuthMiddleware>;

class HeartbeatWorker; // forward declaration

class APIServer 
{
public:
    explicit APIServer(std::shared_ptr<ConfigManager> config_manager, std::shared_ptr<DatabaseManager> db_manager);
    ~APIServer();

    crow::response getConfig();
    crow::response refreshConfig();
    
    void run(int port = 8080);

    void requestForEndpoint(const EndpointConfig& endpoint, const std::unordered_map<std::string, std::string>& pathParams = {});

private:
    void createApp();
    void setupRoutes();
    void setupCORS();
    void setupHeartbeat();
    void handleDynamicRequest(const crow::request& req, crow::response& res);
    crow::response generateOpenAPIDoc();
    
    FlapiApp app;
    std::shared_ptr<ConfigManager> configManager;
    std::shared_ptr<DatabaseManager> dbManager;
    std::shared_ptr<OpenAPIDocGenerator> openAPIDocGenerator;
    std::shared_ptr<HeartbeatWorker> heartbeatWorker;
    RequestHandler requestHandler;
};

} // namespace flapi