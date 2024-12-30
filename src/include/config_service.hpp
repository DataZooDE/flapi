#pragma once

#include <crow.h>
#include "crow/middlewares/cors.h"
#include <memory>

#include "api_server.hpp"
#include "config_manager.hpp"
#include "request_handler.hpp"

namespace flapi {

class ConfigService {
public:
    ConfigService(std::shared_ptr<ConfigManager> config_manager, 
                 std::shared_ptr<RequestHandler> request_handler);

    void registerRoutes(FlapiApp& app);

    std::shared_ptr<ConfigManager> config_manager;
    std::shared_ptr<RequestHandler> request_handler;

    // Route handlers
    crow::response getProjectConfig(const crow::request& req);
    crow::response updateProjectConfig(const crow::request& req);
    
    crow::response listEndpoints(const crow::request& req);
    crow::response createEndpoint(const crow::request& req);
    
    crow::response getEndpointConfig(const crow::request& req, const std::string& path);
    crow::response updateEndpointConfig(const crow::request& req, const std::string& path);
    crow::response deleteEndpoint(const crow::request& req, const std::string& path);
    
    crow::response getEndpointTemplate(const crow::request& req, const std::string& path);
    crow::response updateEndpointTemplate(const crow::request& req, const std::string& path);
    
    crow::response expandTemplate(const crow::request& req, const std::string& path);
    crow::response testTemplate(const crow::request& req, const std::string& path);
    
    crow::response getCacheConfig(const crow::request& req, const std::string& path);
    crow::response updateCacheConfig(const crow::request& req, const std::string& path);
    
    crow::response getCacheTemplate(const crow::request& req, const std::string& path);
    crow::response updateCacheTemplate(const crow::request& req, const std::string& path);
    
    crow::response refreshCache(const crow::request& req, const std::string& path);
    
    crow::response getSchema(const crow::request& req);
    crow::response refreshSchema(const crow::request& req);

    // Helper methods
    crow::json::wvalue endpointConfigToJson(const EndpointConfig& config);
    EndpointConfig jsonToEndpointConfig(const crow::json::rvalue& json);
};

} // namespace flapi
