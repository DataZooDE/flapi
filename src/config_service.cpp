#include <filesystem>
#include <fstream>
#include <sstream>

#include "config_service.hpp"
#include "embedded_ui.hpp"
#include "path_utils.hpp"

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
        ([this](const crow::request& req, const std::string& slug) {
            const std::string path = PathUtils::slugToPath(slug);
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
        ([this](const crow::request& req, const std::string& slug) {
            const std::string path = PathUtils::slugToPath(slug);
            if (req.method == crow::HTTPMethod::Get)
                return getEndpointTemplate(req, path);
            else
                return updateEndpointTemplate(req, path);
        });

    CROW_ROUTE(app, "/api/v1/_config/endpoints/<string>/template/expand")
        .methods("POST"_method)
        ([this](const crow::request& req, const std::string& slug) {
            const std::string path = PathUtils::slugToPath(slug);
            return expandTemplate(req, path);
        });

    CROW_ROUTE(app, "/api/v1/_config/endpoints/<string>/template/test")
        .methods("POST"_method)
        ([this](const crow::request& req, const std::string& slug) {
            const std::string path = PathUtils::slugToPath(slug);
            return testTemplate(req, path);
        });

    // Cache routes
    CROW_ROUTE(app, "/api/v1/_config/endpoints/<string>/cache")
        .methods("GET"_method, "PUT"_method)
        ([this](const crow::request& req, const std::string& slug) {
            const std::string path = PathUtils::slugToPath(slug);
            if (req.method == crow::HTTPMethod::Get)
                return getCacheConfig(req, path);
            else
                return updateCacheConfig(req, path);
        });

    CROW_ROUTE(app, "/api/v1/_config/endpoints/<string>/cache/template")
        .methods("GET"_method, "PUT"_method)
        ([this](const crow::request& req, const std::string& slug) {
            const std::string path = PathUtils::slugToPath(slug);
            if (req.method == crow::HTTPMethod::Get)
                return getCacheTemplate(req, path);
            else
                return updateCacheTemplate(req, path);
        });

    CROW_ROUTE(app, "/api/v1/_config/endpoints/<string>/cache/refresh")
        .methods("POST"_method)
        ([this](const crow::request& req, const std::string& slug) {
            const std::string path = PathUtils::slugToPath(slug);
            return refreshCache(req, path);
        });

    CROW_ROUTE(app, "/api/v1/_config/endpoints/<string>/cache/gc")
        .methods("POST"_method)
        ([this](const crow::request& req, const std::string& slug) {
            const std::string path = PathUtils::slugToPath(slug);
            return performGarbageCollection(req, path);
        });

    // DuckLake audit routes
    CROW_ROUTE(app, "/api/v1/_config/endpoints/<string>/cache/audit")
        .methods("GET"_method)
        ([this](const crow::request& req, const std::string& slug) {
            const std::string path = PathUtils::slugToPath(slug);
            return getCacheAuditLog(req, path);
        });

    CROW_ROUTE(app, "/api/v1/_config/cache/audit")
        .methods("GET"_method)
        ([this](const crow::request& req) {
            return getAllCacheAuditLogs(req);
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
    return config_manager->serializeEndpointConfig(config, EndpointJsonStyle::HyphenCase);
}

EndpointConfig ConfigService::jsonToEndpointConfig(const crow::json::rvalue& json) {
    return config_manager->deserializeEndpointConfig(json);
}

// Implement remaining endpoint handlers...
crow::response ConfigService::updateEndpointConfig(const crow::request& req, const std::string& path) {
    try {
        auto json = crow::json::load(req.body);
        if (!json) {
            return crow::response(400, "Invalid JSON");
        }

        auto updated_config = jsonToEndpointConfig(json);

        if (updated_config.urlPath != path) {
            return crow::response(400, "URL path in config does not match endpoint path");
        }

        if (!config_manager->replaceEndpoint(updated_config)) {
            return crow::response(404, "Endpoint not found");
        }

        return crow::response(200);
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

crow::response ConfigService::deleteEndpoint(const crow::request& req, const std::string& path) {
    try {
        if (!config_manager->removeEndpointByPath(path)) {
            return crow::response(404, "Endpoint not found");
        }

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
        auto template_path = resolveTemplatePath(endpoint->templateSource);
        std::ifstream file(template_path);
        if (!file.is_open()) {
            return crow::response(500, "Could not open template file: " + template_path.string());
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
        auto template_path = resolveTemplatePath(endpoint->templateSource);
        std::ofstream file(template_path);
        if (!file.is_open()) {
            return crow::response(500, "Could not open template file for writing: " + template_path.string());
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
            // Convert all parameter values to strings
            std::string value;
            if (param.t() == crow::json::type::String) {
                value = param.s();
            } else if (param.t() == crow::json::type::Number) {
                // Try integer first, then double
                try {
                    value = std::to_string(param.i());
                } catch (...) {
                    value = std::to_string(param.d());
                }
            } else if (param.t() == crow::json::type::True) {
                value = "true";
            } else if (param.t() == crow::json::type::False) {
                value = "false";
            } else if (param.t() == crow::json::type::Null) {
                value = "";
            } else {
                // For objects and arrays, just use empty string for now
                value = "";
            }
            params[param.key()] = value;
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
            // Convert all parameter values to strings
            std::string value;
            if (param.t() == crow::json::type::String) {
                value = param.s();
            } else if (param.t() == crow::json::type::Number) {
                // Try integer first, then double
                try {
                    value = std::to_string(param.i());
                } catch (...) {
                    value = std::to_string(param.d());
                }
            } else if (param.t() == crow::json::type::True) {
                value = "true";
            } else if (param.t() == crow::json::type::False) {
                value = "false";
            } else if (param.t() == crow::json::type::Null) {
                value = "";
            } else {
                // For objects and arrays, just use empty string for now
                value = "";
            }
            params[param.key()] = value;
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
        auto* endpoint = config_manager->getEndpointForPath(path);
        if (!endpoint) {
            return crow::response(404, "Endpoint not found");
        }

        crow::json::wvalue response;
        response["enabled"] = endpoint->cache.enabled;
        response["table"] = endpoint->cache.table;
        response["schema"] = endpoint->cache.schema;
        if (endpoint->cache.schedule) {
            response["schedule"] = endpoint->cache.schedule.value();
        }
        if (!endpoint->cache.primary_keys.empty()) {
            auto pk_list = crow::json::wvalue::list();
            for (const auto& pk : endpoint->cache.primary_keys) {
                pk_list.push_back(pk);
            }
            response["primary-key"] = std::move(pk_list);
        }
        if (endpoint->cache.cursor) {
            crow::json::wvalue cursor;
            cursor["column"] = endpoint->cache.cursor->column;
            cursor["type"] = endpoint->cache.cursor->type;
            response["cursor"] = std::move(cursor);
        }
        if (endpoint->cache.rollback_window) {
            response["rollback-window"] = endpoint->cache.rollback_window.value();
        }
        if (endpoint->cache.retention.keep_last_snapshots || endpoint->cache.retention.max_snapshot_age) {
            crow::json::wvalue retention;
            if (endpoint->cache.retention.keep_last_snapshots) {
                retention["keep_last_snapshots"] = static_cast<int64_t>(endpoint->cache.retention.keep_last_snapshots.value());
            }
            if (endpoint->cache.retention.max_snapshot_age) {
                retention["max_snapshot_age"] = endpoint->cache.retention.max_snapshot_age.value();
            }
            response["retention"] = std::move(retention);
        }
        if (endpoint->cache.delete_handling) {
            response["delete-handling"] = endpoint->cache.delete_handling.value();
        }
        if (endpoint->cache.template_file) {
            response["template-file"] = endpoint->cache.template_file.value();
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

        auto* endpoint = config_manager->getEndpointForPath(path);
        if (!endpoint) {
            return crow::response(404, "Endpoint not found");
        }

        auto& cache = const_cast<CacheConfig&>(endpoint->cache);
        bool enabled = json["enabled"].b();
        if (enabled) {
            auto table_key = json.has("table") ? json["table"].s() : endpoint->cache.table;
            auto schema_key = json.has("schema") ? json["schema"].s() : endpoint->cache.schema;
            cache.enabled = true;
            cache.table = table_key;
            cache.schema = schema_key;
        } else {
            cache.enabled = false;
        }
        if (json.has("schedule")) {
            cache.schedule = json["schedule"].s();
        }
        if (json.has("primary-key")) {
            cache.primary_keys.clear();
            for (const auto& pk : json["primary-key"]) {
                cache.primary_keys.push_back(pk.s());
            }
        }
        if (json.has("cursor")) {
            CacheConfig::CursorConfig cursor;
            cursor.column = json["cursor"]["column"].s();
            cursor.type = json["cursor"]["type"].s();
            cache.cursor = cursor;
        }
        if (json.has("rollback-window")) {
            cache.rollback_window = json["rollback-window"].s();
        }
        if (json.has("retention")) {
            const auto& retention = json["retention"];
            if (retention.has("keep_last_snapshots")) {
                cache.retention.keep_last_snapshots = static_cast<std::size_t>(retention["keep_last_snapshots"].i());
            }
            if (retention.has("max_snapshot_age")) {
                cache.retention.max_snapshot_age = retention["max_snapshot_age"].s();
            }
        }
        if (json.has("delete-handling")) {
            cache.delete_handling = json["delete-handling"].s();
        }
        if (json.has("template-file")) {
            cache.template_file = json["template-file"].s();
        }

        return crow::response(200);
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

crow::response ConfigService::getCacheTemplate(const crow::request& req, const std::string& path) {
    try {
        auto* endpoint = config_manager->getEndpointForPath(path);
        if (!endpoint) {
            return crow::response(404, "Endpoint not found");
        }
        if (!endpoint->cache.enabled || !endpoint->cache.template_file) {
            return crow::response(400, "Cache template not configured for this endpoint");
        }

        auto cache_path = resolveTemplatePath(endpoint->cache.template_file.value());
        std::ifstream file(cache_path);
        if (!file.is_open()) {
            return crow::response(500, "Could not open cache template file: " + cache_path.string());
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

        auto* endpoint = config_manager->getEndpointForPath(path);
        if (!endpoint) {
            return crow::response(404, "Endpoint not found");
        }
        if (!endpoint->cache.enabled || !endpoint->cache.template_file) {
            return crow::response(400, "Cache template not configured for this endpoint");
        }

        auto cache_path = resolveTemplatePath(endpoint->cache.template_file.value());
        std::ofstream file(cache_path);
        if (!file.is_open()) {
            return crow::response(500, "Could not open cache template file for writing: " + cache_path.string());
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

crow::response ConfigService::performGarbageCollection(const crow::request& req, const std::string& path) {
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

        try {
            // Trigger garbage collection
            cache_manager->performGarbageCollection(config_manager, *endpoint, {});
            return crow::response(200, "Garbage collection completed");
        } catch (const std::exception& e) {
            return crow::response(400, std::string("Garbage collection failed: ") + e.what());
        }
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

std::filesystem::path ConfigService::resolveTemplatePath(const std::string& source) const {
    std::filesystem::path template_path(source);
    if (template_path.is_absolute()) {
        return template_path;
    }

    if (!config_manager) {
        return template_path;
    }

    std::filesystem::path base_path(config_manager->getTemplatePath());
    return (base_path / template_path).lexically_normal();
}

crow::response ConfigService::getSchema(const crow::request& req) {
    try {
        // Parse query parameters
        auto url_params = req.url_params;
        bool tables_only = url_params.get("tables") != nullptr;
        bool connections_only = url_params.get("connections") != nullptr;
        std::string specific_connection = url_params.get("connection") ? url_params.get("connection") : "";
        
        // Get database manager instance
        auto db_manager = DatabaseManager::getInstance();

        // For now, return a simple response based on what's requested
        crow::json::wvalue response;
        
        if (connections_only) {
            // Return connection information
            response["connections"] = crow::json::wvalue::list();
            // TODO: Implement actual connection listing
            return crow::response(200, response);
        }
        
        if (tables_only) {
            // Return table information in the format expected by CLI
            response["tables"] = crow::json::wvalue::list();
            // TODO: Implement actual table listing
            return crow::response(200, response);
        }

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
        if (result.data.t() == crow::json::type::Null) {
            return crow::response(200, response);
        }

        // Build the tree structure from flat results
        for (size_t i = 0; i < result.data.size(); ++i) {
            const auto& row = crow::json::load(result.data[i].dump());
            
            // Safely extract string values, handling NULL
            auto safe_string = [](const crow::json::rvalue& val) -> std::string {
                if (val.t() == crow::json::type::String) {
                    return val.s();
                }
                return "";
            };
            
            // Safely extract boolean values, handling NULL
            auto safe_bool = [](const crow::json::rvalue& val) -> bool {
                if (val.t() == crow::json::type::True) {
                    return true;
                } else if (val.t() == crow::json::type::False) {
                    return false;
                }
                return false;
            };
            
            const std::string schema_name = safe_string(row["schema_name"]);
            const std::string table_name = safe_string(row["table_name"]);
            const std::string column_name = safe_string(row["column_name"]);
            const std::string data_type = safe_string(row["data_type"]);
            const bool is_nullable = safe_bool(row["is_nullable"]);
            const bool is_view = safe_bool(row["is_view"]);

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

crow::response ConfigService::getCacheAuditLog(const crow::request& req, const std::string& path) {
    try {
        const auto* endpoint = config_manager->getEndpointForPath(path);
        if (!endpoint) {
            return crow::response(404, "Endpoint not found");
        }

        if (!endpoint->cache.enabled) {
            return crow::response(400, "Cache not enabled for this endpoint");
        }

        const auto& ducklakeConfig = config_manager->getDuckLakeConfig();
        if (!ducklakeConfig.enabled) {
            return crow::response(400, "DuckLake not enabled");
        }

        auto db_manager = DatabaseManager::getInstance();
        const std::string catalog = ducklakeConfig.alias;
        
        std::string auditQuery = R"(
            SELECT event_id, endpoint_path, cache_table, cache_schema, sync_type, status, message,
                   snapshot_id, rows_affected, sync_started_at, sync_completed_at, duration_ms
            FROM )" + catalog + R"(.audit.sync_events 
            WHERE endpoint_path = ')" + path + R"('
            ORDER BY sync_started_at DESC
            LIMIT 100
        )";

        std::map<std::string, std::string> params;
        auto result = db_manager->executeDuckLakeQuery(auditQuery, params);
        
        return crow::response(200, result.data.dump());
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

crow::response ConfigService::getAllCacheAuditLogs(const crow::request& req) {
    try {
        const auto& ducklakeConfig = config_manager->getDuckLakeConfig();
        if (!ducklakeConfig.enabled) {
            return crow::response(400, "DuckLake not enabled");
        }

        auto db_manager = DatabaseManager::getInstance();
        const std::string catalog = ducklakeConfig.alias;
        
        std::string auditQuery = R"(
            SELECT event_id, endpoint_path, cache_table, cache_schema, sync_type, status, message,
                   snapshot_id, rows_affected, sync_started_at, sync_completed_at, duration_ms
            FROM )" + catalog + R"(.audit.sync_events 
            ORDER BY sync_started_at DESC
            LIMIT 500
        )";

        std::map<std::string, std::string> params;
        auto result = db_manager->executeDuckLakeQuery(auditQuery, params);
        
        return crow::response(200, result.data.dump());
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

} // namespace flapi
