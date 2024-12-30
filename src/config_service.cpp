#include <filesystem>
#include <fstream>
#include <sstream>

#include "config_service.hpp"

namespace flapi {

ConfigService::ConfigService(std::shared_ptr<ConfigManager> config_manager,
                             std::shared_ptr<RequestHandler> request_handler)
    : config_manager(config_manager), request_handler(request_handler) {}

void ConfigService::registerRoutes(FlapiApp& app) {
    CROW_LOG_INFO << "Registering config routes";

    // Project configuration routes
    CROW_ROUTE(app, "/api/v1/_config/project")
        .methods("GET"_method, "PUT"_method)
        ([this](const crow::request& req) {
            if (req.method == crow::HTTPMethod::Get)
                return getProjectConfig(req);
            else
                return updateProjectConfig(req);
        });

    // Endpoints configuration routes
    CROW_ROUTE(app, "/api/v1/_config/endpoints")
        .methods("GET"_method, "POST"_method)
        ([this](const crow::request& req) {
            if (req.method == crow::HTTPMethod::Get)
                return listEndpoints(req);
            else
                return createEndpoint(req);
        });

    CROW_ROUTE(app, "/api/v1/_config/endpoints/<string>")
        .methods("GET"_method, "PUT"_method, "DELETE"_method)
        ([this](const crow::request& req, const std::string& path) {
            switch (req.method) {
                case crow::HTTPMethod::Get:
                    return getEndpointConfig(req, path);
                case crow::HTTPMethod::Put:
                    return updateEndpointConfig(req, path);
                case crow::HTTPMethod::Delete:
                    return deleteEndpoint(req, path);
                default:
                    return crow::response(405);
            }
        });

    // Template routes
    CROW_ROUTE(app, "/api/v1/_config/endpoints/<string>/template")
        .methods("GET"_method, "PUT"_method)
        ([this](const crow::request& req, const std::string& path) {
            if (req.method == crow::HTTPMethod::Get)
                return getEndpointTemplate(req, path);
            else
                return updateEndpointTemplate(req, path);
        });

    CROW_ROUTE(app, "/api/v1/_config/endpoints/<string>/template/expand")
        .methods("POST"_method)
        ([this](const crow::request& req, const std::string& path) {
            return expandTemplate(req, path);
        });

    CROW_ROUTE(app, "/api/v1/_config/endpoints/<string>/template/test")
        .methods("POST"_method)
        ([this](const crow::request& req, const std::string& path) {
            return testTemplate(req, path);
        });

    // Cache routes
    CROW_ROUTE(app, "/api/v1/_config/endpoints/<string>/cache")
        .methods("GET"_method, "PUT"_method)
        ([this](const crow::request& req, const std::string& path) {
            if (req.method == crow::HTTPMethod::Get)
                return getCacheConfig(req, path);
            else
                return updateCacheConfig(req, path);
        });

    CROW_ROUTE(app, "/api/v1/_config/endpoints/<string>/cache/template")
        .methods("GET"_method, "PUT"_method)
        ([this](const crow::request& req, const std::string& path) {
            if (req.method == crow::HTTPMethod::Get)
                return getCacheTemplate(req, path);
            else
                return updateCacheTemplate(req, path);
        });

    CROW_ROUTE(app, "/api/v1/_config/endpoints/<string>/cache/refresh")
        .methods("POST"_method)
        ([this](const crow::request& req, const std::string& path) {
            return refreshCache(req, path);
        });

    // Schema routes
    CROW_ROUTE(app, "/api/v1/_config/schema")
        .methods("GET"_method)
        ([this](const crow::request& req) {
            return getSchema(req);
        });

    CROW_ROUTE(app, "/api/v1/_config/schema/refresh")
        .methods("POST"_method)
        ([this](const crow::request& req) {
            return refreshSchema(req);
        });
}

crow::response ConfigService::getProjectConfig(const crow::request& req) {
    try {
        if (!config_manager) {
            return crow::response(500, "Internal server error: Configuration manager is not initialized");
        }
        auto config = config_manager->getFlapiConfig();
        return crow::response(200, config);
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

crow::response ConfigService::updateProjectConfig(const crow::request& req) {
    try {
        auto json = crow::json::load(req.body);
        if (!json)
            return crow::response(400, "Invalid JSON");

        // TODO: Implement project config update logic
        return crow::response(501, "Not implemented");
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

crow::response ConfigService::listEndpoints(const crow::request& req) {
    try {
        auto endpoints = config_manager->getEndpointsConfig();
        return crow::response(200, endpoints);
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

crow::response ConfigService::createEndpoint(const crow::request& req) {
    try {
        auto json = crow::json::load(req.body);
        if (!json)
            return crow::response(400, "Invalid JSON");

        auto endpoint = jsonToEndpointConfig(json);
        config_manager->addEndpoint(endpoint);
        
        return crow::response(201);
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

crow::response ConfigService::getEndpointConfig(const crow::request& req, const std::string& path) {
    try {
        const auto* endpoint = config_manager->getEndpointForPath(path);
        if (!endpoint)
            return crow::response(404, "Endpoint not found");

        return crow::response(200, endpointConfigToJson(*endpoint));
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

// Helper methods for converting between JSON and EndpointConfig
crow::json::wvalue ConfigService::endpointConfigToJson(const EndpointConfig& config) {
    crow::json::wvalue json;
    json["url-path"] = config.urlPath;
    json["template-source"] = config.templateSource;
    
    // Add request fields
    std::vector<crow::json::wvalue> requestFields;
    for (const auto& field : config.requestFields) {
        crow::json::wvalue fieldJson;
        fieldJson["field-name"] = field.fieldName;
        fieldJson["field-in"] = field.fieldIn;
        fieldJson["description"] = field.description;
        fieldJson["required"] = field.required;
        requestFields.push_back(std::move(fieldJson));
    }
    json["request"] = std::move(requestFields);
    
    return json;
}

EndpointConfig ConfigService::jsonToEndpointConfig(const crow::json::rvalue& json) {
    EndpointConfig config;
    config.urlPath = json["url-path"].s();
    config.templateSource = json["template-source"].s();
    
    // Parse request fields
    if (json.has("request")) {
        for (const auto& field : json["request"]) {
            RequestFieldConfig fieldConfig;
            fieldConfig.fieldName = field["field-name"].s();
            fieldConfig.fieldIn = field["field-in"].s();
            fieldConfig.description = field["description"].s();
            fieldConfig.required = field["required"].b();
            config.requestFields.push_back(fieldConfig);
        }
    }
    
    return config;
}

// Implement remaining endpoint handlers...
crow::response ConfigService::updateEndpointConfig(const crow::request& req, const std::string& path) {
    return crow::response(501, "Not implemented");
}

crow::response ConfigService::deleteEndpoint(const crow::request& req, const std::string& path) {
    return crow::response(501, "Not implemented");
}

crow::response ConfigService::getEndpointTemplate(const crow::request& req, const std::string& path) {
    return crow::response(501, "Not implemented");
}

crow::response ConfigService::updateEndpointTemplate(const crow::request& req, const std::string& path) {
    return crow::response(501, "Not implemented");
}

crow::response ConfigService::expandTemplate(const crow::request& req, const std::string& path) {
    return crow::response(501, "Not implemented");
}

crow::response ConfigService::testTemplate(const crow::request& req, const std::string& path) {
    return crow::response(501, "Not implemented");
}

crow::response ConfigService::getCacheConfig(const crow::request& req, const std::string& path) {
    return crow::response(501, "Not implemented");
}

crow::response ConfigService::updateCacheConfig(const crow::request& req, const std::string& path) {
    return crow::response(501, "Not implemented");
}

crow::response ConfigService::getCacheTemplate(const crow::request& req, const std::string& path) {
    return crow::response(501, "Not implemented");
}

crow::response ConfigService::updateCacheTemplate(const crow::request& req, const std::string& path) {
    return crow::response(501, "Not implemented");
}

crow::response ConfigService::refreshCache(const crow::request& req, const std::string& path) {
    return crow::response(501, "Not implemented");
}

crow::response ConfigService::getSchema(const crow::request& req) {
    return crow::response(501, "Not implemented");
}

crow::response ConfigService::refreshSchema(const crow::request& req) {
    return crow::response(501, "Not implemented");
}

} // namespace flapi
