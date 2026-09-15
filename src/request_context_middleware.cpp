#include "request_context_middleware.hpp"

#include "audit_logger.hpp"
#include "config_manager.hpp"
#include "trace_context.hpp"

namespace flapi {

namespace {

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

// A denial is never suppressed.
//
// MCP suppresses the HTTP-level line because it audits at the tool level, but a
// request rejected BEFORE the tool handler runs - Layer-1 authentication or
// authorization, or a transport rate limit - reaches no tool-level emitter. Left
// suppressed, those produce no audit record whatsoever, which is the worst
// failure mode an audit log has: silence on exactly the events a reviewer is
// looking for.
bool isDenial(int status_code) {
    return status_code == 401 || status_code == 403 || status_code == 429;
}

void emitAuditLine(const std::shared_ptr<AuditLogger>& logger, const RequestContext& rc) {
    if (rc.audit_suppressed && !isDenial(rc.status_code)) {
        return;   // a richer emitter already logged this operation
    }
    if (isProbeRoute(rc.raw_path)) {
        return;
    }
    if (!logger || !logger->isEnabled()) {
        return;
    }
    logger->log(auditEventFrom(rc));
}

}  // namespace

void RequestContextMiddleware::setConfigManager(std::shared_ptr<ConfigManager> config_manager) {
    config_manager_ = std::move(config_manager);
    // Resolve the logger once at bootstrap. getAuditLogger() initialises lazily
    // and is not synchronised, so resolving it here - on the single-threaded
    // startup path - keeps every Crow worker off that race, and keeps the hot
    // path free of shared_ptr refcount traffic when audit is disabled.
    audit_logger_ = config_manager_ ? config_manager_->getAuditLogger() : nullptr;
}

void RequestContextMiddleware::before_handle(crow::request& req, crow::response& res, context& ctx) {
    ctx.started = true;
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

    // W3C trace context from the HTTP headers. MCP requests may override this
    // from params._meta once the JSON-RPC body is parsed (SEP-414 precedence).
    const auto header_ids = parseTraceparent(req.get_header_value("traceparent"),
                                             req.get_header_value("tracestate"),
                                             req.get_header_value("baggage"));
    if (header_ids.valid()) {
        ctx.rc.setTraceId(header_ids.trace_id);
        ctx.rc.setSpanId(header_ids.span_id);
        ctx.rc.sampled = header_ids.sampled();
        ctx.rc.trace_context_source = contextSourceName(ContextSource::Header);
    }

    RequestContextScope::activate(&ctx.rc);

    // Always server-minted; an inbound X-Request-Id is never honoured, or a
    // client could forge collisions and inject into log lines.
    res.set_header("X-Request-Id", std::string(ctx.rc.requestIdView()));
}

void RequestContextMiddleware::after_handle(crow::request&, crow::response& res, context& ctx) {
    finish(res, ctx);
}

void RequestContextMiddleware::finish(crow::response& res, context& ctx) {
    // Crow can call after_handle when before_handle never ran for this request:
    // handle_url() short-circuits an unmatched route by setting
    // need_to_call_after_handlers_ and calling complete_request() directly,
    // WITHOUT running handle() - and ctx_ is reset inside handle(), not
    // handle_url() (crow/http_connection.h:110-119, :143). Without this guard
    // that path audits a default-constructed context: an empty request id, no
    // method, and a latency equal to the connection's age.
    if (!ctx.started || ctx.finished) {
        return;
    }
    ctx.finished = true;
    ctx.rc.status_code = res.code;

    // Instrumentation must never fail a request, and must never leave the
    // ambient pointer set on a pooled worker. Clearing happens whatever the
    // emitter does; the emitter's failures are counted, not propagated.
    struct ClearGuard {
        ~ClearGuard() { RequestContextScope::clear(); }
    } clear_guard;

    try {
        emitAuditLine(audit_logger_, ctx.rc);
    } catch (...) {
        instrumentation_failures_.fetch_add(1, std::memory_order_relaxed);
    }
}


}  // namespace flapi
