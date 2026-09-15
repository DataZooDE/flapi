#include "request_context_middleware.hpp"

#include "audit_logger.hpp"
#include "config_manager.hpp"

namespace flapi {

namespace {

// Set by setConfigManager so the static finishActiveRequest - which has no
// instance to reach through - can still emit an audit line. Written once during
// server bootstrap, read on request threads.
std::shared_ptr<ConfigManager>* g_config_manager = nullptr;

// crow::method_name returns std::string by value, which would allocate on every
// request. RequestContext stores a const char* into static storage instead, so
// map the enum ourselves. Unknown verbs collapse to "_OTHER", matching HTTP
// semantic conventions and keeping the attribute's cardinality bounded.
const char* staticMethodName(crow::HTTPMethod method) {
    switch (method) {
        case crow::HTTPMethod::Get:     return "GET";
        case crow::HTTPMethod::Post:    return "POST";
        case crow::HTTPMethod::Put:     return "PUT";
        case crow::HTTPMethod::Delete:  return "DELETE";
        case crow::HTTPMethod::Patch:   return "PATCH";
        case crow::HTTPMethod::Head:    return "HEAD";
        case crow::HTTPMethod::Options: return "OPTIONS";
        case crow::HTTPMethod::Connect: return "CONNECT";
        case crow::HTTPMethod::Trace:   return "TRACE";
        default:                        return "_OTHER";
    }
}

// Health and liveness probes are excluded from the audit log.
//
// In Kubernetes these are the highest-volume route in the deployment by a wide
// margin and carry no security or compliance information whatsoever - auditing
// them would bury the entries an operator actually needs under probe noise, and
// inflate the log by orders of magnitude. This is the same reasoning (and the
// same route list) as the tracing exclude_routes default.
bool isProbeRoute(std::string_view path) {
    return path == "/health" || path == "/health/live" || path == "/mcp/health";
}

void emitAuditLine(const RequestContext& rc) {
    if (rc.audit_suppressed) {
        return;   // a richer emitter already logged this operation
    }
    if (isProbeRoute(rc.raw_path)) {
        return;
    }
    if (!g_config_manager || !*g_config_manager) {
        return;
    }
    auto logger = (*g_config_manager)->getAuditLogger();
    if (!logger || !logger->isEnabled()) {
        return;
    }
    logger->log(auditEventFrom(rc));
}

}  // namespace

void RequestContextMiddleware::setConfigManager(std::shared_ptr<ConfigManager> config_manager) {
    config_manager_ = std::move(config_manager);
    static std::shared_ptr<ConfigManager> holder;
    holder = config_manager_;
    g_config_manager = &holder;
}

void RequestContextMiddleware::before_handle(crow::request& req, crow::response& res, context& ctx) {
    ctx.finished = false;
    ctx.rc = RequestContext{};
    ctx.rc.t0 = std::chrono::steady_clock::now();
    RequestContext::mintRequestId(ctx.rc.request_id);

    ctx.rc.http_method = staticMethodName(req.method);
    ctx.rc.raw_path = req.url;
    // Route resolution deliberately does NOT happen here. AuthMiddleware and
    // handleDynamicRequest already resolve the endpoint, and resolving a third
    // time would add a full O(N) scan to every request - including /health, which
    // does no resolution at all today. The handler writes the template back into
    // this context once it knows it.
    // route_template already defaults to "<unmatched>".

    RequestContextScope::activate(&ctx.rc);

    // Always server-minted; an inbound X-Request-Id is never honoured, or a
    // client could forge collisions and inject into log lines.
    res.set_header("X-Request-Id", std::string(ctx.rc.requestIdView()));
}

void RequestContextMiddleware::after_handle(crow::request&, crow::response& res, context& ctx) {
    finish(res, ctx);
}

void RequestContextMiddleware::finish(crow::response& res, context& ctx) {
    if (ctx.finished) {
        return;   // idempotent: a middleware may have completed this already
    }
    ctx.finished = true;

    ctx.rc.status_code = res.code;
    emitAuditLine(ctx.rc);
    RequestContextScope::clear();
}

void RequestContextMiddleware::finishActiveRequest(int status_code) {
    RequestContext* rc = RequestContextScope::current();
    if (rc == nullptr) {
        return;
    }
    rc->status_code = status_code;
    emitAuditLine(*rc);
    RequestContextScope::clear();
}

}  // namespace flapi
