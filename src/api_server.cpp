#include <thread>
#include <yaml-cpp/yaml.h>

#include "api_server.hpp"
#include "in_flight_registry.hpp"
#include "request_context.hpp"
#include "auth_middleware.hpp"
#include "database_manager.hpp"
#include "flapi_telemetry.hpp"

#include <chrono>
#include "config_service.hpp"
#include "config_tool_adapter.hpp"
#include "open_api_doc_generator.hpp"
#include "open_api_page.hpp"
#include "rate_limit_middleware.hpp"
#include "mcp_session_manager.hpp"
#include "mcp_client_capabilities.hpp"

namespace flapi {

APIServer::APIServer(std::shared_ptr<ConfigManager> cm, 
                     std::shared_ptr<DatabaseManager> db_manager, 
                     bool config_service_enabled,
                     const std::string& config_service_token)
    : configManager(cm), dbManager(db_manager), openAPIDocGenerator(std::make_shared<OpenAPIDocGenerator>(cm, db_manager)), requestHandler(dbManager, cm), startedAt(std::chrono::steady_clock::now())
{
    // Initialize MCP session manager
    mcpSessionManager = std::make_shared<MCPSessionManager>();

    // Initialize MCP client capabilities detector
    mcpCapabilitiesDetector = std::make_shared<MCPClientCapabilitiesDetector>();

    // Create ConfigToolAdapter for configuration management tools
    // Only initialize if config-service is enabled
    std::unique_ptr<ConfigToolAdapter> config_tool_adapter;
    if (config_service_enabled) {
        try {
            config_tool_adapter = std::make_unique<ConfigToolAdapter>(cm, db_manager);
            CROW_LOG_INFO << "ConfigToolAdapter initialized - config MCP tools available";
        } catch (const std::exception& e) {
            CROW_LOG_WARNING << "Failed to initialize ConfigToolAdapter: " << e.what();
            config_tool_adapter = nullptr;
        }
    } else {
        CROW_LOG_DEBUG << "Config service disabled - config MCP tools will not be available";
        config_tool_adapter = nullptr;
    }

    // Initialize MCP route handlers (always enabled in unified configuration)
    // Port will be passed when registering routes
    CROW_LOG_INFO << "Initializing MCP Route Handlers...";
    try {
        mcpRouteHandlers = std::make_unique<MCPRouteHandlers>(cm, db_manager, mcpSessionManager,
                                                               mcpCapabilitiesDetector,
                                                               std::move(config_tool_adapter));
        CROW_LOG_DEBUG << "MCP Route Handlers initialized successfully";
    } catch (const std::exception& e) {
        CROW_LOG_ERROR << "Failed to initialize MCP Route Handlers: " << e.what();
        mcpRouteHandlers = nullptr;
    }

    CROW_LOG_INFO << "APIServer MCP Route Handlers status: " << (mcpRouteHandlers ? "initialized" : "failed to initialize");

    // Initialize ConfigService with token authentication
    configService = std::make_shared<ConfigService>(configManager, config_service_enabled, config_service_token);
    
    createApp();
    setupRoutes();
    setupCORS();
    setupHeartbeat();

    CROW_LOG_INFO << "APIServer initialized with MCP support";
}

APIServer::~APIServer() {
    heartbeatWorker->stop();
}

void APIServer::createApp() 
{
    // Configure middlewares
    app.get_middleware<RequestContextMiddleware>().setConfigManager(configManager);
    app.get_middleware<RateLimitMiddleware>().setConfig(configManager);
    app.get_middleware<AuthMiddleware>().initialize(configManager);
}

void APIServer::setupRoutes() {
    CROW_LOG_INFO << "Setting up routes...";
    CROW_LOG_INFO << "APIServer setupRoutes called - MCP Route Handlers available: " << (mcpRouteHandlers ? "yes" : "no");

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

    CROW_ROUTE(app, "/health/live")
        .methods("GET"_method)
        ([this]() {
            return getLiveHealth();
        });

    CROW_ROUTE(app, "/health")
        .methods("GET"_method)
        ([this]() {
            return getHealth();
        });

    configService->setDocGenerator(openAPIDocGenerator);
    configService->registerRoutes(app);

    // /config returns the whole configuration: every endpoint, and every
    // connection's `init` SQL and properties. It was served to anyone who
    // could reach the port - no token, and registered even when the config
    // service was switched off - so a default deployment disclosed its own
    // credentials on request. Verified against a running server: a password in
    // connections.*.properties came back in the body of an unauthenticated GET.
    //
    // It now requires the same token as every other configuration route.
    // Properties are redacted at the source as well (config_manager.cpp), but
    // redaction alone is not enough here: a credential can sit inside the
    // `init` SQL - the shipped SAP example puts a PASSWD literal there - and
    // no key-name predicate can find it.
    CROW_ROUTE(app, "/config")
        .methods("GET"_method)
        ([this](const crow::request& req, crow::response& res) {
            if (!configService || !configService->validateToken(req)) {
                res = crow::response(401, "Unauthorized: configuration access requires the config-service token");
                res.end();
                return;
            }
            res = getConfig();
            res.end();
        });

    CROW_ROUTE(app, "/config")
        .methods("DELETE"_method)
        ([this](const crow::request& req, crow::response& res) {
            if (!configService || !configService->validateToken(req)) {
                res = crow::response(401, "Unauthorized: configuration access requires the config-service token");
                res.end();
                return;
            }
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

    // Register MCP routes BEFORE the catch-all route
    // This ensures /mcp/jsonrpc matches before the catch-all /<path> route
    if (mcpRouteHandlers) {
        mcpRouteHandlers->registerRoutes(app, configManager->getHttpPort());
    } else {
        CROW_LOG_WARNING << "MCP Route Handlers not initialized, skipping MCP route registration";
    }

    // Endpoint route (supports GET, POST, PUT, PATCH, DELETE)
    // Must be registered LAST so specific routes (like /mcp/jsonrpc) match first
    CROW_ROUTE(app, "/<path>")
        .methods("GET"_method, "POST"_method, "PUT"_method, "PATCH"_method, "DELETE"_method)
        ([this](const crow::request& req, crow::response& res, std::string path) {
            handleDynamicRequest(req, res);
            // Note: don't call res.end() here - handlers already call it
        });

    CROW_LOG_INFO << "Routes set up completed";
}

void APIServer::setupCORS() {
    const auto& cors_cfg = configManager->getCorsConfig();

    auto& cors = app.get_middleware<crow::CORSHandler>();
    auto& rules = cors.global();

    // Crow's built-in CORSHandler covers methods + headers. Origin handling
    // is intentionally left as the wildcard default here so it doesn't
    // conflict with FlapiCorsMiddleware, which sets ACAO per request based
    // on the configured allowlist (see cors_middleware.cpp).
    if (cors_cfg.allow_headers.empty()) {
        rules.headers("*");
    } else {
        for (const auto& h : cors_cfg.allow_headers) {
            rules.headers(h);
        }
    }

    if (cors_cfg.allow_methods.empty()) {
        rules.methods("GET"_method, "POST"_method, "PUT"_method,
                      "PATCH"_method, "DELETE"_method);
    } else {
        for (const auto& m : cors_cfg.allow_methods) {
            if (m == "GET") {
                rules.methods("GET"_method);
            } else if (m == "POST") {
                rules.methods("POST"_method);
            } else if (m == "PUT") {
                rules.methods("PUT"_method);
            } else if (m == "PATCH") {
                rules.methods("PATCH"_method);
            } else if (m == "DELETE") {
                rules.methods("DELETE"_method);
            } else if (m == "OPTIONS") {
                rules.methods("OPTIONS"_method);
            } else if (m == "HEAD") {
                rules.methods("HEAD"_method);
            }
        }
    }

    // Hand the allowlist to the flapi-owned middleware so it can resolve
    // the per-request `Access-Control-Allow-Origin` value.
    auto& flapi_cors = app.get_middleware<FlapiCorsMiddleware>();
    flapi_cors.initialize(configManager);
}

void APIServer::setupHeartbeat() {
    heartbeatWorker = std::make_shared<HeartbeatWorker>(configManager, *this);
    heartbeatWorker->start();
}

void APIServer::handleDynamicRequest(const crow::request& req, crow::response& res)
{
    const auto t0 = std::chrono::steady_clock::now();
    std::string path = req.url;
    // Match endpoint by both path and HTTP method
    std::string method = crow::method_name(req.method);
    const auto endpoint = configManager->getEndpointForPathAndMethod(path, method);

    // The middleware deliberately does not resolve routes (that would be a third
    // O(N) scan per request, and would tax /health, which resolves nothing
    // today). The handler already knows the template, so it writes it back.
    if (auto* rc = RequestContextScope::current(); rc != nullptr && endpoint != nullptr) {
        // Copy, not a view: `endpoint` is pinned only for this function, while
        // route_template is read later in after_handle.
        rc->route_template_storage = endpoint->urlPath;
        rc->route_template = rc->route_template_storage;

        // Per-endpoint capture override, so the middleware can resolve the
        // effective tier. A global `capture: off` still wins over it.
        if (endpoint->mcp_tool) {
            rc->endpoint_capture = endpoint->mcp_tool->response.tracing_capture;
        }

        // Declared request fields only - never the raw query string. A field the
        // endpoint declares is part of its contract; an arbitrary query parameter
        // is not, and exporting one would leak whatever a caller appended.
        // Values are still redacted and clamped downstream, and are emitted ONLY
        // at the payload tier.
        for (const auto& field : endpoint->request_fields) {
            // Query fields only. A field declared as `field-in: body` or
            // `header` is not in the query string, and reading it from there
            // would attribute an unrelated caller-supplied value to its name.
            if (!field.fieldIn.empty() && field.fieldIn != "query") {
                continue;
            }
            const auto value = req.url_params.get(field.fieldName);
            if (value != nullptr) {
                rc->audit_params.emplace_back(field.fieldName, value);
            }
        }
    }

    // Emit one rest_endpoint_served with the ROUTE TEMPLATE (never the filled
    // path), status class, duration, and whether the endpoint is cache-backed.
    // Errors are emitted with an enumerated class only (no message/SQL/body).
    auto emit_telemetry = [&](const std::string& route_template, bool cache_hit) {
        const double duration_ms =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0).count();
        auto& telemetry = flapi::GlobalTelemetry();
        telemetry.restEndpointServed(method, route_template, res.code, duration_ms, cache_hit);
        if (res.code >= 500) {
            telemetry.error("server_error", "rest_endpoint_served", route_template);
        } else if (res.code == 400) {
            telemetry.error("bad_request", "rest_endpoint_served", route_template);
        }
    };

    if (!endpoint) {
        res.code = 404;
        res.body = "Not Found";
        res.end();
        emit_telemetry("<unmatched>", false);
        return;
    }

    std::vector<std::string> paramNames;
    std::map<std::string, std::string> pathParams;

    bool matched = RouteTranslator::matchAndExtractParams(endpoint->urlPath, path, paramNames, pathParams);

    if (!matched) {
        res.code = 404;
        res.body = "Not Found";
        res.end();
        emit_telemetry("<unmatched>", false);
        return;
    }

    // Build auth params from middleware context for template variable injection
    auto& auth_ctx = app.get_context<AuthMiddleware>(req);
    std::map<std::string, std::string> auth_params;
    if (auth_ctx.authenticated) {
        auth_params["__auth_username"] = auth_ctx.username;
        auth_params["__auth_email"]    = auth_ctx.email;
        auth_params["__auth_type"]     = auth_ctx.auth_type;
        auth_params["__auth_authenticated"] = "true";
        std::string roles;
        for (const auto& r : auth_ctx.roles) {
            if (!roles.empty()) {
                roles += ",";
            }
            roles += r;
        }
        auth_params["__auth_roles"] = roles;
    }

    requestHandler.handleRequest(req, res, *endpoint, pathParams, auth_params);

    // flapi has no per-request cache hit/miss signal (the "cache" is a
    // materialized DuckLake table the template queries); the honest bounded
    // value is whether this endpoint is cache-backed. See TELEMETRY.md.
    emit_telemetry(endpoint->urlPath, dbManager->isCacheEnabled(*endpoint));
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

crow::response APIServer::getLiveHealth() {
    crow::json::wvalue health;
    health["status"] = "live";
    return crow::response(200, health);
}

crow::response APIServer::getHealth() {
    crow::json::wvalue health;
    const auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - startedAt);
    health["uptime_s"] = static_cast<int64_t>(uptime.count());

    CacheManager::CacheReadinessSummary summary;
    auto cache_manager = getCacheManager();
    if (cache_manager) {
        summary = cache_manager->getReadinessSummary();
    }

    health["caches"]["total"] = summary.total;
    health["caches"]["ready"] = summary.ready;
    health["caches"]["failed"] = summary.failed;

    // A stalled request is checked BEFORE cache state, because it is the more
    // serious condition: caches degrade loudly, a wedged backend does not.
    //
    // This measures the symptom rather than probing the resource. A probe would
    // have to touch each connection, and a probe against a wedged one blocks
    // forever - leaking a thread per health check. Request age costs nothing
    // and catches any stall, whatever caused it.
    const int stall_timeout_s = configManager ? configManager->getStallTimeoutSeconds() : 0;
    const auto oldest = InFlightRegistry::oldestAge(std::chrono::steady_clock::now());
    health["requests"]["in_flight"] = static_cast<int64_t>(InFlightRegistry::inFlight());
    health["requests"]["oldest_ms"] = static_cast<int64_t>(oldest.count());

    if (stall_timeout_s > 0 && oldest >= std::chrono::seconds(stall_timeout_s)) {
        health["status"] = "stalled";
        health["stalled_after_s"] = stall_timeout_s;
        // Deliberately blunt: a request outliving this budget means something
        // downstream stopped answering, and the instance should be taken out of
        // rotation rather than handed more traffic it cannot serve.
        return crow::response(503, health);
    }

    if (summary.failed > 0) {
        health["status"] = "degraded";
        std::vector<crow::json::wvalue> failed;
        for (const auto& cache : summary.failed_caches) {
            crow::json::wvalue item;
            item["table"] = cache.table;
            item["schema"] = cache.schema;
            item["error"] = cache.error;
            failed.push_back(std::move(item));
        }
        health["failed"] = std::move(failed);
        return crow::response(503, health);
    }

    if (summary.ready < summary.total) {
        health["status"] = "starting";
        std::vector<crow::json::wvalue> pending;
        for (const auto& cache : summary.pending_caches) {
            crow::json::wvalue item;
            item["table"] = cache.table;
            item["schema"] = cache.schema;
            pending.push_back(std::move(item));
        }
        health["pending"] = std::move(pending);
        return crow::response(503, health);
    }

    health["status"] = "ready";
    return crow::response(200, health);
}

crow::response APIServer::generateOpenAPIDoc() {
    YAML::Node doc = openAPIDocGenerator->generateDoc(app);
    
    // Convert YAML::Node to string
    std::stringstream ss;
    ss << doc;
    
    return crow::response(200, ss.str());
}

std::uint16_t APIServer::serverThreadCount(unsigned hardware_concurrency) {
    // 8 total -> 7 io threads. Enough that a single stuck query leaves the
    // instance answering, small enough to be unremarkable on a 1-vCPU
    // container. Crow clamps anything below 2 itself.
    constexpr unsigned kFloor = 8;
    const unsigned threads = hardware_concurrency > kFloor ? hardware_concurrency : kFloor;
    // Crow takes a uint16_t; a machine reporting more cores than that is not
    // a reason to wrap around to nothing.
    constexpr unsigned kMax = 64;
    return static_cast<std::uint16_t>(threads < kMax ? threads : kMax);
}

void APIServer::run(int port) {
    if (port > 0) {
        configManager->setHttpPort(port);
    }

    const auto& https = configManager->getHttpsConfig();
    const std::string bind_host = configManager->getHttpHost();
    if (https.enabled) {
        CROW_LOG_INFO << "HTTPS enabled: serving TLS on " << bind_host << ":" << configManager->getHttpPort();
        CROW_LOG_DEBUG << "  cert: " << https.ssl_cert_file;
        CROW_LOG_DEBUG << "  key:  " << https.ssl_key_file;
        app.bindaddr(bind_host)
           .port(configManager->getHttpPort())
           .server_name("flAPI")
           .concurrency(serverThreadCount(std::thread::hardware_concurrency()))
           .use_compression(crow::compression::GZIP)
           .ssl_file(https.ssl_cert_file, https.ssl_key_file)
           .run();
    } else {
        CROW_LOG_INFO << "Server starting on " << bind_host << ":" << configManager->getHttpPort() << "...";
        app.bindaddr(bind_host)
           .port(configManager->getHttpPort())
           .server_name("flAPI")
           .concurrency(serverThreadCount(std::thread::hardware_concurrency()))
           .use_compression(crow::compression::GZIP)
           .run();
    }
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

void APIServer::stop() {
    heartbeatWorker->stop();
    app.stop();
}

std::shared_ptr<CacheManager> APIServer::getCacheManager() const {
    return dbManager->getCacheManager();
}

std::shared_ptr<DatabaseManager> APIServer::getDatabaseManager() const {
    return dbManager;
}

} // namespace flapi
