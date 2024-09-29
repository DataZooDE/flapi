#pragma once

#include <crow.h>
#include "crow/middlewares/cors.h"

#include "config_manager.hpp"
#include "request_handler.hpp"
#include "rate_limit_middleware.hpp"
#include "auth_middleware.hpp"
#include "route_translator.hpp"
#include "database_manager.hpp"
#include "open_api_doc_generator.hpp"

namespace flapi {

class APIServer 
{
public:
    explicit APIServer(std::shared_ptr<ConfigManager> config_manager, std::shared_ptr<DatabaseManager> db_manager);

    crow::response getConfig();
    crow::response refreshConfig();
    void run(int port = 8080);

    

private:
    void createApp();
    void setupRoutes();
    void setupCORS();
    void handleDynamicRequest(const crow::request& req, crow::response& res);
    crow::response generateOpenAPIDoc();
    
    crow::App<crow::CORSHandler, RateLimitMiddleware, AuthMiddleware> app;
    std::shared_ptr<ConfigManager> configManager;
    std::shared_ptr<DatabaseManager> dbManager;
    std::shared_ptr<OpenAPIDocGenerator> openAPIDocGenerator;
    RequestHandler requestHandler;
};

} // namespace flapi