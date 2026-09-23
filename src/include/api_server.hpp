#pragma once

#include <cstdint>

#include <crow.h>
#include "crow/middlewares/cors.h"
#include "crow/compression.h"

#include "auth_middleware.hpp"
#include "flapi_app.hpp"
#include "handler_pool.hpp"
#include "config_manager.hpp"
#include "cors_middleware.hpp"
#include "database_manager.hpp"
#include "heartbeat_worker.hpp"
#include "open_api_doc_generator.hpp"
#include "rate_limit_middleware.hpp"
#include "request_handler.hpp"
#include "route_translator.hpp"
#include "mcp_route_handlers.hpp"
#include "mcp_session_manager.hpp"
#include "mcp_client_capabilities.hpp"

namespace flapi {


class ConfigService;   // forward declaration
class HeartbeatWorker; // forward declaration

class APIServer 
{
public:
    explicit APIServer(std::shared_ptr<ConfigManager> config_manager, 
                      std::shared_ptr<DatabaseManager> db_manager, 
                      bool config_service_enabled = false,
                      const std::string& config_service_token = "");
    ~APIServer();

    crow::response getConfig();
    crow::response refreshConfig();
    crow::response getLiveHealth();
    crow::response getHealth();
    
    void run(int port = 8080);

    /// How many threads to hand Crow.
    ///
    /// Crow runs a request handler on the io thread that owns its connection
    /// and dedicates one thread to accepting, so `concurrency` threads give
    /// `concurrency - 1` io threads. On a 1- or 2-vCPU container -
    /// Cloud Run's default - `hardware_concurrency()` therefore leaves a
    /// SINGLE io thread, and one slow synchronous query blocks every other
    /// connection on the instance, `/health` included (#120).
    ///
    /// io threads spend nearly all their time waiting on sockets, so
    /// over-providing them costs thread stacks rather than CPU. The floor
    /// makes head-of-line blocking improbable on a small instance instead of
    /// certain. It does not make it impossible - that needs handlers off the
    /// io threads, which is what #120 tracks.
    static std::uint16_t serverThreadCount(unsigned hardware_concurrency);


    void stop();

    void requestForEndpoint(const EndpointConfig& endpoint, const std::unordered_map<std::string, std::string>& pathParams = {});
    
    // Getters for heartbeat worker
    std::shared_ptr<CacheManager> getCacheManager() const;
    std::shared_ptr<DatabaseManager> getDatabaseManager() const;

private:
    void createApp();
    void setupRoutes();
    void setupCORS();
    void setupHeartbeat();
    void handleDynamicRequest(const crow::request& req, crow::response& res);
    crow::response generateOpenAPIDoc();
    
    FlapiApp app;

    // Worker threads that run request handlers off Crow's io threads (#120).
    // Null when the offload is disabled, in which case handlers run inline.
    //
    // DECLARED AFTER `app` deliberately. Members are destroyed in reverse
    // order, so this joins its workers before the io_contexts and connections
    // they post completions into are torn down. Declared before `app`, a
    // worker still mid-query when the process is stopping would post to a
    // destroyed io_service and then drop a keepalive holding a connection
    // whose socket belonged to it.
    std::unique_ptr<HandlerPool> handlerPool;
    /// stop() is reachable from the signal supervisor and from main.
    std::mutex stop_mutex_;
    /// Set by stop() and never cleared; run() refuses to start when it is set.
    std::atomic<bool> stop_requested_{false};
    /// The drain happens once; app.stop() is re-issued on every stop() call.
    bool drained_ = false;
    std::shared_ptr<ConfigManager> configManager;
    std::shared_ptr<ConfigService> configService;
    std::shared_ptr<DatabaseManager> dbManager;
    std::shared_ptr<OpenAPIDocGenerator> openAPIDocGenerator;
    std::shared_ptr<HeartbeatWorker> heartbeatWorker;
    std::unique_ptr<MCPRouteHandlers> mcpRouteHandlers;
    std::shared_ptr<MCPSessionManager> mcpSessionManager;
    std::shared_ptr<MCPClientCapabilitiesDetector> mcpCapabilitiesDetector;
    RequestHandler requestHandler;
    std::chrono::steady_clock::time_point startedAt;
};

} // namespace flapi
