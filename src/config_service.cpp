#include <filesystem>
#include <fstream>
#include <sstream>

#include "config_service.hpp"
#include "embedded_ui.hpp"

namespace flapi {

// Define content types for static files
const std::unordered_map<std::string, std::string> ConfigService::content_types = {
    {".html", "text/html"},
    {".js", "application/javascript"},
    {".css", "text/css"},
    {".json", "application/json"},
    {".png", "image/png"},
    {".jpg", "image/jpeg"},
    {".gif", "image/gif"},
    {".svg", "image/svg+xml"},
    {".ico", "image/x-icon"}
};

ConfigService::ConfigService(std::shared_ptr<ConfigManager> config_manager)
    : config_manager(config_manager) {}

std::string ConfigService::get_content_type(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    auto it = content_types.find(ext);
    return it != content_types.end() ? it->second : "application/octet-stream";
}

void ConfigService::registerRoutes(FlapiApp& app) {
    CROW_LOG_INFO << "Registering config routes";

    // Serve the UI static files
    CROW_ROUTE(app, "/ui/<path>")
    ([this](const crow::request& req, std::string path) {
        auto content = embedded_ui::get_file_content("/index.html");
        if (content.empty()) {
            return crow::response(404);
        }
        
        // Find the end of the HTML content (</html>)
        size_t html_end = content.find("</html>");
        if (html_end == std::string::npos) {
            return crow::response(500, "Invalid HTML content");
        }
        html_end += 7; // Length of "</html>"
        
        // Only return the HTML content, not any trailing data
        auto html_content = content.substr(0, html_end);
        
        auto resp = crow::response(200, std::string(html_content));
        resp.set_header("Content-Type", "text/html; charset=UTF-8");
        resp.set_header("Cache-Control", "no-cache");
        return resp;
    });

    // Serve root as index.html
    CROW_ROUTE(app, "/ui")
    ([this](const crow::request& req) {
        auto content = embedded_ui::get_file_content("/index.html");
        if (content.empty()) {
            return crow::response(404);
        }
        
        // Find the end of the HTML content (</html>)
        size_t html_end = content.find("</html>");
        if (html_end == std::string::npos) {
            return crow::response(500, "Invalid HTML content");
        }
        html_end += 7; // Length of "</html>"
        
        // Only return the HTML content, not any trailing data
        auto html_content = content.substr(0, html_end);
        
        auto resp = crow::response(200, std::string(html_content));
        resp.set_header("Content-Type", "text/html; charset=UTF-8");
        resp.set_header("Cache-Control", "no-cache");
        return resp;
    });

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
    try {
        auto json = crow::json::load(req.body);
        if (!json) {
            return crow::response(400, "Invalid JSON");
        }

        // Find the endpoint
        auto* endpoint = config_manager->getEndpointForPath(path);
        if (!endpoint) {
            return crow::response(404, "Endpoint not found");
        }

        // Convert JSON to EndpointConfig
        auto updated_config = jsonToEndpointConfig(json);
        
        // Validate that the path matches
        if (updated_config.urlPath != path) {
            return crow::response(400, "URL path in config does not match endpoint path");
        }

        // Update the endpoint in config_manager
        // First remove the old endpoint
        auto& endpoints = const_cast<std::vector<EndpointConfig>&>(config_manager->getEndpoints());
        endpoints.erase(
            std::remove_if(endpoints.begin(), endpoints.end(),
                [&path](const EndpointConfig& e) { return e.urlPath == path; }),
            endpoints.end()
        );

        // Then add the updated endpoint
        config_manager->addEndpoint(updated_config);

        return crow::response(200);
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

crow::response ConfigService::deleteEndpoint(const crow::request& req, const std::string& path) {
    try {
        // Find the endpoint
        auto* endpoint = config_manager->getEndpointForPath(path);
        if (!endpoint) {
            return crow::response(404, "Endpoint not found");
        }

        // Remove the endpoint from config_manager
        auto& endpoints = const_cast<std::vector<EndpointConfig>&>(config_manager->getEndpoints());
        endpoints.erase(
            std::remove_if(endpoints.begin(), endpoints.end(),
                [&path](const EndpointConfig& e) { return e.urlPath == path; }),
            endpoints.end()
        );

        return crow::response(200);
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

crow::response ConfigService::getEndpointTemplate(const crow::request& req, const std::string& path) {
    try {
        // Find the endpoint
        auto* endpoint = config_manager->getEndpointForPath(path);
        if (!endpoint) {
            return crow::response(404, "Endpoint not found");
        }

        // Read the template file
        std::ifstream file(endpoint->templateSource);
        if (!file.is_open()) {
            return crow::response(500, "Could not open template file: " + endpoint->templateSource);
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        
        crow::json::wvalue response;
        response["template"] = buffer.str();
        return crow::response(200, response);
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

crow::response ConfigService::updateEndpointTemplate(const crow::request& req, const std::string& path) {
    try {
        auto json = crow::json::load(req.body);
        if (!json || !json.has("template")) {
            return crow::response(400, "Invalid JSON: missing 'template' field");
        }

        // Find the endpoint
        auto* endpoint = config_manager->getEndpointForPath(path);
        if (!endpoint) {
            return crow::response(404, "Endpoint not found");
        }

        // Write the template to file
        std::ofstream file(endpoint->templateSource);
        if (!file.is_open()) {
            return crow::response(500, "Could not open template file for writing: " + endpoint->templateSource);
        }

        file << json["template"].s();
        if (!file.good()) {
            return crow::response(500, "Error writing template to file");
        }

        return crow::response(200);
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

crow::response ConfigService::expandTemplate(const crow::request& req, const std::string& path) {
    try {
        auto json = crow::json::load(req.body);
        if (!json || !json.has("parameters")) {
            return crow::response(400, "Invalid JSON: missing 'parameters' field");
        }

        // Find the endpoint
        auto* endpoint = config_manager->getEndpointForPath(path);
        if (!endpoint) {
            return crow::response(404, "Endpoint not found");
        }

        // Convert parameters to map
        std::map<std::string, std::string> params;
        for (const auto& param : json["parameters"]) {
            params[param.key()] = param.s();
        }

        // Use SQLTemplateProcessor to expand the template
        auto sql_processor = std::make_shared<SQLTemplateProcessor>(config_manager);
        std::string expanded = sql_processor->loadAndProcessTemplate(*endpoint, params);

        crow::json::wvalue response;
        response["expanded"] = expanded;
        return crow::response(200, response);
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

crow::response ConfigService::testTemplate(const crow::request& req, const std::string& path) {
    try {
        auto json = crow::json::load(req.body);
        if (!json || !json.has("parameters")) {
            return crow::response(400, "Invalid JSON: missing 'parameters' field");
        }

        // Find the endpoint
        auto* endpoint = config_manager->getEndpointForPath(path);
        if (!endpoint) {
            return crow::response(404, "Endpoint not found");
        }

        if (endpoint->connection.empty()) {
            return crow::response(400, "Endpoint has no database connection configured");
        }

        // Convert parameters to map
        std::map<std::string, std::string> params;
        for (const auto& param : json["parameters"]) {
            params[param.key()] = param.s();
        }

        // Get database manager instance
        auto db_manager = DatabaseManager::getInstance();

        // Execute the query with a limit for safety
        params["limit"] = "10";  // Force a limit for test queries
        params["offset"] = "0";
        
        try {
            auto result = db_manager->executeQuery(*endpoint, params);
            
            // Create base response
            crow::json::wvalue response;
            response["success"] = true;
            
            // Handle empty result case
            if (result.data.t() == crow::json::type::Null || result.data.size() == 0) {
                response["columns"] = std::vector<std::string>();
                response["rows"] = std::vector<crow::json::wvalue>();
                return crow::response(200, response);
            }

            response["rows"] = std::move(result.data);
            response["columns"] = response["rows"][0].keys();
            
            return crow::response(200, response);

        } catch (const std::exception& e) {
            return crow::response(400, std::string("SQL execution error: ") + e.what());
        }
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

crow::response ConfigService::getCacheConfig(const crow::request& req, const std::string& path) {
    try {
        // Find the endpoint
        auto* endpoint = config_manager->getEndpointForPath(path);
        if (!endpoint) {
            return crow::response(404, "Endpoint not found");
        }

        // Convert cache config to JSON
        crow::json::wvalue response;
        if (endpoint->cache.enabled) {
            response["enabled"] = true;
            response["refresh-time"] = endpoint->cache.refreshTime;
            response["cache-source"] = endpoint->cache.cacheSource;
            response["cache-schema"] = config_manager->getCacheSchema();
            response["cache-table"] = endpoint->cache.cacheTableName;
        } else {
            response["enabled"] = false;
        }

        return crow::response(200, response);
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

crow::response ConfigService::updateCacheConfig(const crow::request& req, const std::string& path) {
    try {
        auto json = crow::json::load(req.body);
        if (!json) {
            return crow::response(400, "Invalid JSON");
        }

        // Find the endpoint
        auto* endpoint = config_manager->getEndpointForPath(path);
        if (!endpoint) {
            return crow::response(404, "Endpoint not found");
        }

        // Cast endpoint to non-const
        auto& endpoint_non_const = const_cast<EndpointConfig&>(*endpoint);

        // Update cache configuration
        bool enabled = json["enabled"].b();
        if (enabled) {
            // Validate refresh time format first
            if (!json.has("refresh-time")) {
                return crow::response(400, "Missing required field 'refresh-time' when cache is enabled");
            }

            auto refresh_time = json["refresh-time"].s();
            if (!flapi::TimeInterval::parseInterval(refresh_time)) {
                return crow::response(400, "Invalid refresh time format. Expected format: <number>[s|m|h|d] (e.g., 30s, 5m, 2h, 1d)");
            }

            endpoint_non_const.cache.enabled = true;
            endpoint_non_const.cache.refreshTime = refresh_time;
            endpoint_non_const.cache.cacheSource = json["cache-source"].s();
            endpoint_non_const.cache.cacheTableName = json["cache-table"].s();
        } else {
            endpoint_non_const.cache.enabled = false;
        }

        return crow::response(200);
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

crow::response ConfigService::getCacheTemplate(const crow::request& req, const std::string& path) {
    try {
        // Find the endpoint
        auto* endpoint = config_manager->getEndpointForPath(path);
        if (!endpoint) {
            return crow::response(404, "Endpoint not found");
        }

        if (!endpoint->cache.enabled) {
            return crow::response(400, "Cache is not enabled for this endpoint");
        }

        // Read the cache template file
        std::ifstream file(endpoint->cache.cacheSource);
        if (!file.is_open()) {
            return crow::response(500, "Could not open cache template file: " + endpoint->cache.cacheSource);
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        
        crow::json::wvalue response;
        response["template"] = buffer.str();
        return crow::response(200, response);
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

crow::response ConfigService::updateCacheTemplate(const crow::request& req, const std::string& path) {
    try {
        auto json = crow::json::load(req.body);
        if (!json || !json.has("template")) {
            return crow::response(400, "Invalid JSON: missing 'template' field");
        }

        // Find the endpoint
        auto* endpoint = config_manager->getEndpointForPath(path);
        if (!endpoint) {
            return crow::response(404, "Endpoint not found");
        }

        if (!endpoint->cache.enabled) {
            return crow::response(400, "Cache is not enabled for this endpoint");
        }

        // Write the template to file
        std::ofstream file(endpoint->cache.cacheSource);
        if (!file.is_open()) {
            return crow::response(500, "Could not open cache template file for writing: " + endpoint->cache.cacheSource);
        }

        file << json["template"].s();
        if (!file.good()) {
            return crow::response(500, "Error writing template to file");
        }

        return crow::response(200);
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

crow::response ConfigService::refreshCache(const crow::request& req, const std::string& path) {
    try {
        // Find the endpoint
        auto* endpoint = config_manager->getEndpointForPath(path);
        if (!endpoint) {
            return crow::response(404, "Endpoint not found");
        }

        if (!endpoint->cache.enabled) {
            return crow::response(400, "Cache is not enabled for this endpoint");
        }

        // Get database manager instance
        auto db_manager = DatabaseManager::getInstance();
        auto cache_manager = std::make_shared<CacheManager>(db_manager);

        // Prepare empty parameters map for cache refresh
        std::map<std::string, std::string> params;

        try {
            // Trigger cache refresh
            cache_manager->refreshCache(config_manager, *endpoint, params);
            return crow::response(200);
        } catch (const std::exception& e) {
            return crow::response(400, std::string("Cache refresh failed: ") + e.what());
        }
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

crow::response ConfigService::getSchema(const crow::request& req) {
    try {
        // Get database manager instance
        auto db_manager = DatabaseManager::getInstance();

        // Use a single SQL query to get all schema information
        const std::string query = R"SQL(
            WITH schema_tables AS (
                SELECT 
                    s.schema_name,
                    t.table_name,
                    CASE WHEN t.table_type = 'BASE TABLE' THEN false ELSE true END as is_view,
                    c.column_name,
                    c.data_type,
                    c.is_nullable = 'YES' as is_nullable
                FROM information_schema.schemata s
                LEFT JOIN information_schema.tables t 
                    ON s.schema_name = t.table_schema
                LEFT JOIN information_schema.columns c 
                    ON t.table_schema = c.table_schema 
                    AND t.table_name = c.table_name
                WHERE s.schema_name NOT IN ('information_schema', 'pg_catalog')
                ORDER BY s.schema_name, t.table_name, c.ordinal_position
            )
            SELECT DISTINCT * FROM schema_tables
        )SQL";

        auto result = db_manager->executeQuery(query, {}, false);
    
        // Process results into the desired JSON structure
        crow::json::wvalue response;
        if (result.data.t() == crow::json::type::Null) {
            return crow::response(200, response);
        }

        // Build the tree structure from flat results
        for (size_t i = 0; i < result.data.size(); ++i) {
            const auto& row = crow::json::load(result.data[i].dump());
            const auto& schema_name = row["schema_name"].s();
            const auto& table_name = row["table_name"].s();
            const auto& column_name = row["column_name"].s();
            const auto& data_type = row["data_type"].s();
            const bool is_nullable = row["is_nullable"].b();
            const bool is_view = row["is_view"].b();

            // Initialize schema if not exists
            auto schema_names = response.keys();
            if (std::find(schema_names.begin(), schema_names.end(), schema_name) == schema_names.end()) {
                response[schema_name]["tables"] = crow::json::wvalue();
            }

            if (table_name.size() == 0) {
                continue;
            }

            // Initialize table if not exists
            auto table_names = response[schema_name]["tables"].keys();
            if (std::find(table_names.begin(), table_names.end(), table_name) == table_names.end()) {
                response[schema_name]["tables"][table_name]["is_view"] = is_view;
                response[schema_name]["tables"][table_name]["columns"] = crow::json::wvalue();
            }

            if (column_name.size() == 0) {
                continue;
            }

            // Add column info
            auto& columns = response[schema_name]["tables"][table_name]["columns"];
            columns[column_name]["type"] = data_type;
            columns[column_name]["nullable"] = is_nullable;
        }

        return crow::response(200, response);
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

crow::response ConfigService::refreshSchema(const crow::request& req) {
    try {
        // Get database manager instance
        auto db_manager = DatabaseManager::getInstance();

        // Re-initialize database connections from config
        db_manager->initializeDBManagerFromConfig(config_manager);

        return crow::response(200);
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

} // namespace flapi
