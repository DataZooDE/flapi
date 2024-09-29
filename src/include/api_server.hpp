#pragma once

#include <crow.h>

#include "config_manager.hpp"
#include "request_handler.hpp"
#include "rate_limit_middleware.hpp"
#include "auth_middleware.hpp"
#include "route_translator.hpp"
#include "database_manager.hpp"

namespace flapi {

class APIServer 
{
private:
    crow::App<RateLimitMiddleware, AuthMiddleware> app;
    std::shared_ptr<ConfigManager> configManager;
    std::shared_ptr<DatabaseManager> dbManager;
    RequestHandler requestHandler;

public:
    explicit APIServer(std::shared_ptr<ConfigManager> config_manager, std::shared_ptr<DatabaseManager> db_manager);
    void createApp();
    void setupRoutes();
    crow::response getConfig();
    crow::response refreshConfig();
    void run(int port = 8080);

    void handleDynamicRequest(const crow::request& req, crow::response& res);
};

} // namespace flapi