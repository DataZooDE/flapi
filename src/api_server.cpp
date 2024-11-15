#include <yaml-cpp/yaml.h>

#include "api_server.hpp"
#include "auth_middleware.hpp"
#include "database_manager.hpp"
#include "open_api_doc_generator.hpp"
#include "open_api_page.hpp"
#include "rate_limit_middleware.hpp"

namespace flapi {

APIServer::APIServer(std::shared_ptr<ConfigManager> cm, std::shared_ptr<DatabaseManager> db_manager)
    : configManager(cm), dbManager(db_manager), requestHandler(dbManager, cm),
      openAPIDocGenerator(std::make_shared<OpenAPIDocGenerator>(cm, db_manager))
{
    createApp();
    setupRoutes();
    setupCORS();
    setupHeartbeat();
    
    CROW_LOG_INFO << "APIServer initialized";
}

APIServer::~APIServer() {
    heartbeatWorker->stop();
}

void APIServer::createApp() 
{
    // Configure middlewares
    app.get_middleware<RateLimitMiddleware>().setConfig(configManager);
    app.get_middleware<AuthMiddleware>().initialize(configManager);
}

void APIServer::setupRoutes() {
    CROW_LOG_INFO << "Setting up routes...";

    CROW_ROUTE(app, "/")([](){
        CROW_LOG_INFO << "Root route accessed";
        std::string logo = R"(
         ___
     ___( o)>   Welcome to
     \ <_. )    flAPI
      `---'    

    Fast and Flexible API Framework
        powered by DuckDB
    )";
        return crow::response(200, "text/plain", logo);
    });

    CROW_ROUTE(app, "/config")
        .methods("GET"_method)
        ([this](const crow::request& req, crow::response& res) {
            res = getConfig();
            res.end();
        });

    CROW_ROUTE(app, "/config")
        .methods("DELETE"_method)
        ([this](const crow::request& req, crow::response& res) {
            CROW_LOG_INFO << "Config refresh requested";
            res = refreshConfig();
            res.end();
        });

    CROW_ROUTE(app, "/doc")
        .methods("GET"_method)
        ([this]() {
            return generateOpenAPIPage(configManager);
        });

    CROW_ROUTE(app, "/doc.yaml")
        .methods("GET"_method)
        ([this]() {
            return generateOpenAPIDoc();
        });

    // Endpoint route
    CROW_ROUTE(app, "/<path>")
        .methods("GET"_method, "DELETE"_method)
        ([this](const crow::request& req, crow::response& res, std::string path) {
            handleDynamicRequest(req, res);
            res.end();
        });

    CROW_LOG_INFO << "Routes set up completed";
}

void APIServer::setupCORS() {
    auto& cors = app.get_middleware<crow::CORSHandler>();
    cors.global()
        .headers("*")
        .methods("GET"_method, "DELETE"_method);
}

void APIServer::setupHeartbeat() {
    heartbeatWorker = std::make_shared<HeartbeatWorker>(configManager, *this);
    heartbeatWorker->start();
}

void APIServer::handleDynamicRequest(const crow::request& req, crow::response& res) 
{
    std::string path = req.url;
    const auto& endpoint = configManager->getEndpointForPath(path);

    if (!endpoint) {
        res.code = 404;
        res.body = "Not Found";
        res.end();
        return;
    }

    std::vector<std::string> paramNames;
    std::map<std::string, std::string> pathParams;
    
    bool matched = RouteTranslator::matchAndExtractParams(endpoint->urlPath, path, paramNames, pathParams);

    if (!matched) {
        res.code = 404;
        res.body = "Not Found";
        res.end();
        return;
    }

    requestHandler.handleRequest(req, res, *endpoint, pathParams);    
}

crow::response APIServer::getConfig() {
    try {
        crow::json::wvalue config;
        config["flapi"] = configManager->getFlapiConfig();
        config["endpoints"] = configManager->getEndpointsConfig();
        return crow::response(200, config.dump(2));
    } catch (const std::exception& e) {
        CROW_LOG_ERROR << "Error in getConfig: " << e.what();
        return crow::response(500, std::string("Internal Server Error: ") + e.what());
    }
}

crow::response APIServer::refreshConfig() {
    try {
        configManager->refreshConfig();
        return crow::response(200, "Configuration refreshed successfully");
    } catch (const std::exception& e) {
        CROW_LOG_ERROR << "Failed to refresh configuration: " << e.what();
        return crow::response(500, std::string("Failed to refresh configuration: ") + e.what());
    }
}

crow::response APIServer::generateOpenAPIDoc() {
    YAML::Node doc = openAPIDocGenerator->generateDoc(app);
    
    // Convert YAML::Node to string
    std::stringstream ss;
    ss << doc;
    
    return crow::response(200, ss.str());
}

void APIServer::run(int port) {
    if (port > 0) {
        configManager->setHttpPort(port);
    }

    CROW_LOG_INFO << "Server starting on port " << configManager->getHttpPort() << "...";
    app.port(configManager->getHttpPort())
       .server_name("flAPI")
       .multithreaded()
       .run();
}


void APIServer::requestForEndpoint(const EndpointConfig& endpoint, const std::unordered_map<std::string, std::string>& pathParams) 
{
    auto req = crow::request();
    req.method = crow::HTTPMethod::Get;
    req.url = endpoint.urlPath;

    std::stringstream qs;
    for (const auto& [key, value] : pathParams) {
        qs << key << "=" << value << "&";
    }
    req.url_params = qs.str();

    auto res = crow::response();
    app.handle_full(req, res);
}

} // namespace flapi
