#pragma once

#include <crow.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <string>

#include "request_context.hpp"
#include "trace_scope.hpp"
#include "trace_capture_policy.hpp"
#include "tracing_config.hpp"

namespace flapi {

class ConfigManager;
class AuditLogger;

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
// Lifetime contract, verified against the pinned Crow rather than assumed:
//
// after_handle runs on EVERY path. When a middleware completes the response
// inside before_handle (AuthMiddleware's 401 paths do), Crow calls that
// middleware's after_handle and then unwinds through every OUTER middleware
// calling theirs too (crow/middleware.h:151-155). This middleware is index 0, so
// it is always reached. The second pass at http_connection.h:207 is then skipped
// precisely because the unwind already ran them - reading only that line suggests
// the opposite conclusion, which is why this comment cites both.
//
// So completion belongs in after_handle and nowhere else. An earlier version also
// completed explicitly on the 401 path and emitted every 401 twice; the `finished`
// flag below stays as a cheap guard, and a test asserts exactly one audit line.
class RequestContextMiddleware {
public:
    struct context {
        RequestContext rc;       // by value: no allocation, no refcount atomics
        SpanScope span;          // inert and allocation-free when tracing is off
        bool started = false;    // before_handle actually ran for THIS request
        bool finished = false;   // guards the idempotent completion path
    };

    void before_handle(crow::request& req, crow::response& res, context& ctx);
    void after_handle(crow::request& req, crow::response& res, context& ctx);

    // Shared by the server bootstrap so the audit sink is reachable.
    void setConfigManager(std::shared_ptr<ConfigManager> config_manager);

private:
    void finish(crow::response& res, context& ctx);

    std::shared_ptr<ConfigManager> config_manager_;
    // Copied at bootstrap so the hot path touches no shared_ptr and no config
    // lookup. Route exclusion is matched against the raw path.
    std::vector<std::string> excluded_routes_;
    bool tracing_configured_ = false;
    // Copied at bootstrap; the policy itself is immutable after construction.
    const CapturePolicy* capture_policy_ = nullptr;
    bool openinference_ = false;
    // Resolved once at bootstrap: getAuditLogger() initialises lazily without
    // synchronisation, and this middleware runs on every Crow worker.
    std::shared_ptr<AuditLogger> audit_logger_;
    // Instrumentation failures are counted, never propagated into the request.
    static inline std::atomic<std::uint64_t> instrumentation_failures_{0};
};

}  // namespace flapi
