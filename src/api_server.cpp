#include <cstdlib>
#include <future>
#include <thread>
#include <yaml-cpp/yaml.h>

#include "api_server.hpp"
#include "handler_pool.hpp"
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

    // Handlers run off the io threads by default (#120). Sized from the
    // hardware, with a floor so a small instance still has somewhere to put
    // concurrent work, and a bounded queue so a slow backend sheds load
    // instead of growing without limit.
    //
    // FLAPI_DISABLE_HANDLER_OFFLOAD=1 restores the inline path, as an escape
    // hatch for a deployment that hits something this change did not
    // anticipate.
    {
        const char* disabled = std::getenv("FLAPI_DISABLE_HANDLER_OFFLOAD");
        if (disabled != nullptr && std::string(disabled) == "1") {
            CROW_LOG_WARNING << "handler offload disabled; a slow query will block "
                                "other connections on its io thread (#120)";
        } else {
            const unsigned hw = std::thread::hardware_concurrency();
            const std::size_t pool_threads = hw > 4 ? hw : 4;
            handlerPool = std::make_unique<HandlerPool>(pool_threads, pool_threads * 32);
            CROW_LOG_INFO << "handler offload enabled with " << pool_threads
                          << " worker threads";
        }
    }

    // Initialize MCP client capabilities detector
    mcpCapabilitiesDetector = std::make_shared<MCPClientCapabilitiesDetector>();

    // Create ConfigToolAdapter for configuration management tools
    // Only initialize if config-service is enabled
    std::unique_ptr<ConfigToolAdapter> config_tool_adapter;
    if (config_service_enabled) {
        try {
            config_tool_adapter = std::make_unique<ConfigToolAdapter>(cm, db_manager, config_service_token);
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
            // Crow runs this on the io thread that owns the connection, so a
            // slow synchronous query holds that thread for the whole query and
            // every other connection assigned to it waits - GET /health
            // included, which is how a stalled instance is supposed to report
            // itself. Measured on one ~5s query with 40 concurrent probes and
            // a single io thread: 40/40 blocked, 0/40 reported the stall
            // (#120).
            //
            // The work therefore moves to a pool thread and only the
            // completion is posted back to the owning io_service, which is the
            // thread Crow expects to touch the connection's buffers.
            //
            // Every request arriving here came off a socket, so `io_service`
            // and `middleware_context` are both non-null. That is a property
            // of the code now rather than a hope: the one non-HTTP caller -
            // the heartbeat - no longer enters the router at all. See
            // warmEndpoint() for the two crashes that cost, and
            // scripts/check_no_synthetic_router_entry.sh for what stops a
            // future caller reintroducing it.
            if (!handlerPool) {
                handleDynamicRequest(req, res);
                return;
            }

            // Everything the worker needs to reconstitute this request's
            // ambient state. Both are thread-local by design - see
            // SpanScope::Activation and RequestContextScope - so neither
            // survives the hop on its own.
            auto* rc = RequestContextScope::current();
            auto& mw = app.get_context<RequestContextMiddleware>(req);
            // Holds the connection open across the offload. Without it,
            // prepare_buffers() clears the connection's only remaining owners
            // and destroys it before it finishes using itself.
            auto keepalive = res.connection_keepalive;

            // The response must be completed EXACTLY once, on every exit path.
            // Making that a property of the code - remembering to post a
            // completion in each branch - is how a non-std::exception escape
            // left the connection hung: the read loop is paused and the
            // deadline timer cancelled, so nothing else would ever finish it,
            // finish() would never run, and the in-flight slot would never
            // clear, leaving readiness stuck at 503 for the life of the
            // process.
            //
            // So it is a property of the TYPE instead. Whatever happens to the
            // job, this posts something.
            struct Completer {
                crow::response* res;
                const crow::request* req;
                std::shared_ptr<void> keepalive;
                int code = 500;
                std::string body = "Internal Server Error";
                crow::ci_map headers;
                // EVERY field of crow::response a handler can set has to make
                // the hop, not just the obvious three.
                //
                // These were dropped, and dropping `compressed` silently
                // corrupted Arrow IPC responses: the Arrow path sets
                // `compressed = false` (its payload has its own LZ4/ZSTD
                // framing) and an explicit Content-Length. The flag stayed
                // true on the real response, so Crow gzipped the body while
                // the length header described the uncompressed size, and every
                // client got a truncated stream -
                // "IncompleteRead(826858 bytes read, 1851846 more expected)".
                // 15 of 18 Arrow integration tests went red on the offload
                // branch and green on main.
                //
                // Anything added to crow::response in a future version has to
                // be added here too; check_crow_response_fields.sh fails the
                // build if the struct grows a field this does not carry.
#ifdef CROW_ENABLE_COMPRESSION
                bool compressed = true;
#endif
                bool skip_body = false;
                bool manual_length_header = false;
                // crow::response::file_info is deliberately NOT carried -
                // see scripts/check_crow_response_fields.py's NOT_CARRIED.
                bool armed = true;

                void take(crow::response& from) {
                    code = from.code;
                    body = std::move(from.body);
                    headers = std::move(from.headers);
#ifdef CROW_ENABLE_COMPRESSION
                    compressed = from.compressed;
#endif
                    skip_body = from.skip_body;
                    manual_length_header = from.manual_length_header;
                }

                ~Completer() {
                    if (!armed) {
                        return;
                    }
                    asio::post(*req->io_service,
                               [res = res, keepalive = keepalive, code = code,
                                body = std::move(body),
                                headers = std::move(headers),
#ifdef CROW_ENABLE_COMPRESSION
                                compressed = compressed,
#endif
                                skip_body = skip_body,
                                manual_length_header = manual_length_header]() mutable {
                                   res->code = code;
                                   res->body = std::move(body);
                                   for (auto& header : headers) {
                                       res->set_header(header.first, header.second);
                                   }
#ifdef CROW_ENABLE_COMPRESSION
                                   res->compressed = compressed;
#endif
                                   res->skip_body = skip_body;
                                   res->manual_length_header = manual_length_header;
                                   res->end();
                               });
                }
            };

            // The io thread must stop claiming this request the moment the
            // work leaves it. Crow logs "Request:" for the NEXT connection on
            // this thread before that request's before_handle runs, so leaving
            // t_current set here stamps the offloaded request's id and trace id
            // onto another request's log lines - the exact correlation the
            // release advertises.
            const bool queued = handlerPool->submit(
                [this, &req, &res, rc, &mw, keepalive]() mutable {
                    Completer completer{&res, &req, keepalive};

                    RequestContextScope::activate(rc);
                    const auto span_activation = mw.span.activateOnThisThread();

                    crow::response local;
                    try {
                        handleDynamicRequest(req, local);
                        completer.take(local);
                    } catch (const std::exception& e) {
                        CROW_LOG_ERROR << "handler threw off the io thread: " << e.what();
                    } catch (...) {
                        CROW_LOG_ERROR << "handler threw a non-standard exception off the io thread";
                    }

                    RequestContextScope::clearIf(rc);
                    // ~Completer posts the completion, whichever way we leave.
                });

            if (queued) {
                RequestContextScope::clearIf(rc);
                // Release the span's activation on THIS thread while it is
                // still the innermost one, so the context stack detaches in
                // LIFO order. The worker re-activates on its own thread; the
                // span itself stays live and is ended in finish() as before.
                mw.span.suspendActivation();
            }

            if (!queued) {
                // The queue is full. Answering 503 here is the honest reply to
                // more work than this instance can take; silently queueing it
                // would eventually serve a client that stopped waiting.
                CROW_LOG_WARNING << "handler pool is saturated; shedding a request";
                res.code = 503;
                res.set_header("Content-Type", "text/plain");
                res.set_header("Retry-After", "1");
                res.body = "Server is at capacity. Try again shortly.";
                res.end();
            }
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

    // Build auth params from middleware context for template variable injection.
    //
    // Unguarded: this function is reachable only from the router, so `req`
    // came off a socket and `middleware_context` is non-null. It was guarded
    // for a while because the heartbeat synthesised a bare crow::request and
    // fed it to app.handle_full(), which reached this dereference four hundred
    // lines from the offload guard added for the same reason. That caller is
    // gone - see warmEndpoint().
    std::map<std::string, std::string> auth_params;
    auto& auth_ctx = app.get_context<AuthMiddleware>(req);
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
    // Test seam. A test for #120 has to be able to produce the condition the
    // issue is about - a slow query monopolising the io thread a probe needs -
    // and on a many-core machine the floor makes that vanishingly unlikely.
    // Without this the test passes against the un-offloaded server too, which
    // is exactly the trap the first version of it fell into.
    if (const char* override_threads = std::getenv("FLAPI_IO_THREADS")) {
        const int parsed = std::atoi(override_threads);
        if (parsed >= 2 && parsed <= 64) {
            return static_cast<std::uint16_t>(parsed);
        }
    }

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
    // A shutdown requested before we ever bind must not be overtaken by the
    // bind. stop() can run during startup - the signal supervisor is alive
    // before the server is - and Crow's app.stop() is a no-op until run() has
    // assigned its server, so without this check the process would go on to
    // serve with an already-drained handler pool: 503 for every request, for
    // the life of a process that no longer answers SIGTERM.
    if (stop_requested_.load(std::memory_order_acquire)) {
        CROW_LOG_INFO << "shutdown was requested during startup; not starting the server";
        return;
    }

    if (port > 0) {
        configManager->setHttpPort(port);
    }

    const auto& https = configManager->getHttpsConfig();
    const std::string bind_host = configManager->getHttpHost();
    // Crow installs its own asio signal_set for SIGINT/SIGTERM by default,
    // and asio's signal_set REPLACES the sigaction main() installed. So on
    // SIGTERM crow stopped its own io_services, run() returned, and main
    // walked out through exit() - while flapi's handler never ran, the
    // handler pool was never drained, and ~APIServer joined a worker during
    // STATIC DESTRUCTION. That worker was still inside a query and reached
    // QueryExecutor's function-local statics after they had been destroyed:
    //
    //   #0 flapi::unregisterActiveExecutor(std::thread::id)
    //   #1 flapi::QueryExecutor::execute(...)
    //   #8 flapi::HandlerPool::run()
    //   -- main thread: exit() -> ~APIServer -> ~HandlerPool -> join()
    //
    // i.e. SIGTERM during any in-flight query segfaulted. Measured on a plain
    // `kill -TERM` with one slow request running.
    //
    // flapi handles these signals itself, so crow must not: shutdown is
    // ordered (pool drained, then io_services stopped) and happens while the
    // process is still alive.
    app.signal_clear();

    // run_async + wait_for_server_start, NOT run().
    //
    // The flag check at the top of this function is necessary but not
    // sufficient: signal_clear(), the bind and Crow's own setup all happen
    // after it and before Crow publishes `server_`, and app.stop() is a
    // no-op until it does. A SIGTERM inside that window therefore drained the
    // handler pool, stopped nothing, and left a process that served 503 for
    // the rest of its life and needed SIGKILL - three consecutive reviews
    // found this sequence still losable.
    //
    // Waiting until Crow is actually stoppable and re-checking closes it: by
    // then either stop() has already run (and we stop immediately) or it has
    // not, and any later stop() finds a server to stop.
    std::future<void> serving;
    if (https.enabled) {
        CROW_LOG_INFO << "HTTPS enabled: serving TLS on " << bind_host << ":" << configManager->getHttpPort();
        CROW_LOG_DEBUG << "  cert: " << https.ssl_cert_file;
        CROW_LOG_DEBUG << "  key:  " << https.ssl_key_file;
        serving = app.bindaddr(bind_host)
           .port(configManager->getHttpPort())
           .server_name("flAPI")
           .concurrency(serverThreadCount(std::thread::hardware_concurrency()))
           .use_compression(crow::compression::GZIP)
           .ssl_file(https.ssl_cert_file, https.ssl_key_file)
           .run_async();
    } else {
        CROW_LOG_INFO << "Server starting on " << bind_host << ":" << configManager->getHttpPort() << "...";
        serving = app.bindaddr(bind_host)
           .port(configManager->getHttpPort())
           .server_name("flAPI")
           .concurrency(serverThreadCount(std::thread::hardware_concurrency()))
           .use_compression(crow::compression::GZIP)
           .run_async();
    }

    // No waiter thread, and no unbounded wait.
    //
    // Two things can happen while crow starts, and both were mishandled:
    //
    //  - it FAILS. Crow constructs its Server - which binds in its member
    //    initialiser list - and only then publishes it, so EADDRINUSE throws
    //    before wait_for_server_start() can ever return. Waiting on that
    //    blocked forever on a condition variable nothing would signal;
    //    app.stop() is a no-op with no server, so not even SIGTERM recovered.
    //
    //  - a STOP is requested first. app.stop() before crow publishes its
    //    server does nothing, so the process went on to serve with an already
    //    drained handler pool: 503 for every request, for the life of a
    //    process that no longer answered SIGTERM.
    //
    // Watching wait_for_server_start() on a helper thread answered both and
    // introduced a third: on the failure path that helper can never be woken
    // (notify_server_start() is private to crow), so it was detached and left
    // blocked on a condition variable that ~Crow then destroyed.
    //
    // Polling the future answers both with no extra thread. Re-issuing
    // app.stop() each tick is harmless before crow publishes its server and
    // effective from the moment it does, so a stop requested at any point
    // during startup takes effect without anyone having to detect WHEN crow
    // became stoppable.
    while (serving.wait_for(std::chrono::milliseconds(20)) != std::future_status::ready) {
        if (stop_requested_.load(std::memory_order_acquire)) {
            app.stop();
        }
    }

    // .get(), not .wait(): a startup failure is an exception, and discarding
    // it would turn "port already in use" into a silent, serverless process
    // that answers nothing and cannot be stopped. main catches it, logs, and
    // shuts down cleanly.
    serving.get();
}

void APIServer::warmEndpoint(const EndpointConfig& endpoint)
{
    // The heartbeat's warm-up: the ONLY non-HTTP entry into the serving path,
    // and deliberately not an HTTP one.
    //
    // Its predecessor, requestForEndpoint(), synthesised a bare crow::request
    // and handed it to app.handle_full() - i.e. it entered the router as if it
    // had come off a socket. A default-constructed crow::request has
    // `io_service == nullptr` and `middleware_context == nullptr`, and that
    // cost the same null dereference twice in one release:
    //
    //   1. The #120 handler offload posted its completion to `*req.io_service`
    //      and read `app.get_context<RequestContextMiddleware>(req)`. Both
    //      dereferenced null on every heartbeat tick, and the offload is ON by
    //      default - so every deployment with a heartbeat-enabled endpoint was
    //      hitting it.
    //   2. The fix guarded the offload and routed such requests INLINE to
    //      handleDynamicRequest() - which builds its auth params from
    //      `app.get_context<AuthMiddleware>(req)`, four hundred lines from the
    //      guard. The crash moved rather than went:
    //
    //        #0 flapi::APIServer::handleDynamicRequest(...)
    //        #4 flapi::APIServer::requestForEndpoint(...)
    //        #5 flapi::HeartbeatWorker::performHeartbeat(...)
    //
    // Both were then guarded on `req.middleware_context != nullptr`, which is
    // the wrong shape: the router was being defended from a caller with no
    // business being there, and every future line that reasonably assumes "a
    // request here came off a socket" was another latent instance (#141).
    //
    // So the heartbeat now calls the handler directly. Nothing synthetic
    // reaches the router, both guards are gone, and
    // scripts/check_no_synthetic_router_entry.sh fails the build if a
    // handle_full() call reappears anywhere in src/.
    //
    // RequestHandler::handleRequest takes its auth context as an EXPLICIT
    // argument and reads only req.method, req.url, req.url_params, req.body and
    // req.get_header_value() - never middleware_context, never io_service. A
    // synthesised request is therefore safe here, and only here, which is why
    // it is a function-local that nothing else can get hold of.
    crow::request req;

    // GET unconditionally, even for a POST/PUT/DELETE endpoint. Warming must
    // not mutate anything; the point is to pay the connection, extension,
    // template-render and query-plan costs once so the first real caller does
    // not. (This matches what requestForEndpoint() did, which forced Get too.)
    req.method = crow::HTTPMethod::Get;
    // Read by createNextUrl() and isCacheDetailsRequest(). The route template
    // is the honest value here - there is no filled path.
    req.url = endpoint.urlPath;

    // `heartbeat.params` was parsed from day one (config_manager.cpp,
    // parseEndpointHeartbeat) and never read: requestForEndpoint() was only
    // ever called with its defaulted empty map, so a documented knob silently
    // did nothing. They are passed as pathParams because that is the slot
    // combineParameters() folds into the template context as `params.*` - the
    // same place a path or query parameter lands.
    std::map<std::string, std::string> params(endpoint.heartbeat.params.begin(),
                                              endpoint.heartbeat.params.end());

    // No auth params. A warm-up has no authenticated principal by
    // construction, so the empty auth context is the CORRECT one, not merely
    // the available one - the same conclusion the deleted guard had reached.
    crow::response res;
    requestHandler.handleRequest(req, res, endpoint, params);

    // Nothing consumes the response, so a failure would otherwise be silent.
    // No telemetry is emitted either: a warm-up is not a request anybody made,
    // and counting it as rest_endpoint_served would inflate the endpoint's
    // traffic with a tick of the worker interval.
    if (res.code >= 400) {
        CROW_LOG_WARNING << "heartbeat warm-up of " << endpoint.urlPath
                         << " returned " << res.code;
    }
}

void APIServer::stop() {
    // Reachable from the signal supervisor and from main. Serialised so the
    // two cannot interleave, and so the second caller does not return while
    // the first is still draining.
    std::lock_guard<std::mutex> lock(stop_mutex_);

    // Recorded BEFORE anything else, and never cleared: run() checks it and
    // refuses to start.
    //
    // A latch that returned early here was worse than useless. Crow's
    // app.stop() is a no-op until run() has assigned its server, so a SIGTERM
    // arriving in the startup window drained the pool, set the latch, and did
    // NOT stop anything - and then run() went on to serve with a shut-down
    // handler pool, so every request got 503 forever while a second SIGTERM
    // returned at the latch. The supervisor loop added for exactly this case
    // could not help.
    stop_requested_.store(true, std::memory_order_release);

    if (!drained_) {
        drained_ = true;
        heartbeatWorker->stop();

    // Before app.stop(), not after. The pool's shutdown drains rather than
    // drops, and that promise is only worth anything while the io_services
    // those jobs post their completions into are still alive - after
    // app.stop() no posted completion can run, so a "drained" job would
    // finish its query and then have nowhere to deliver the response.
    //
    // submit() already refuses while stopping, so a request arriving during
    // shutdown gets 503 rather than being queued into a closing server.
        if (handlerPool) {
            handlerPool->shutdown();
        }
    }

    // Issued on EVERY call, not just the first: the first may have run before
    // Crow had a server to stop.
    app.stop();
}

std::shared_ptr<CacheManager> APIServer::getCacheManager() const {
    return dbManager->getCacheManager();
}

std::shared_ptr<DatabaseManager> APIServer::getDatabaseManager() const {
    return dbManager;
}

} // namespace flapi
