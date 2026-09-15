#pragma once

#include <crow.h>

#include <memory>
#include <string>

#include "request_context.hpp"

namespace flapi {

class ConfigManager;

// The leftmost middleware in FlapiApp.
//
// Owns the per-request RequestContext: mints the request id, starts the single
// clock, makes the context ambient for the log handler and the inner pipeline,
// sets X-Request-Id on the response, and emits the REST audit line.
//
// It must be leftmost because Crow runs before_handle in declaration order and
// after_handle in reverse, so first position is the only one that brackets rate
// limiting and auth. Instrumenting the request handler instead would miss every
// 401, 403, 429, every static route and every 404 - which is most of what an
// operator actually asks about.
//
// It carries no tracing dependency and is useful with FLAPI_WITH_TRACING=OFF:
// request identity, log correlation and REST audit coverage all work without an
// exporter. The HTTP SERVER span is added here later.
//
// IMPORTANT - Crow does not always call after_handle. When a middleware completes
// the response inside before_handle (AuthMiddleware's 401 paths do), Crow takes
// the complete_request() branch with need_to_call_after_handlers_ still false
// (crow/http_connection.h:183-207). Such call sites MUST call
// RequestContextMiddleware::finishActiveRequest() before res.end(), and
// after_handle is written to be idempotent so the double path is harmless.
class RequestContextMiddleware {
public:
    struct context {
        RequestContext rc;       // by value: no allocation, no refcount atomics
        bool finished = false;   // guards the idempotent completion path
    };

    void before_handle(crow::request& req, crow::response& res, context& ctx);
    void after_handle(crow::request& req, crow::response& res, context& ctx);

    // Shared by the server bootstrap so the audit sink is reachable.
    void setConfigManager(std::shared_ptr<ConfigManager> config_manager);

    // Completes the request that is ambient on this thread. For middlewares that
    // end a response inside before_handle and would otherwise never reach
    // after_handle. Safe to call when nothing is ambient.
    static void finishActiveRequest(int status_code);

private:
    void finish(crow::response& res, context& ctx);

    std::shared_ptr<ConfigManager> config_manager_;
};

}  // namespace flapi
