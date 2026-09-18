#include "request_context_middleware.hpp"

#include "audit_logger.hpp"
#include "config_manager.hpp"
#include "trace_context.hpp"
#include "flapi_tracing.hpp"
#include "trace_semconv.hpp"

#include <crow/json.h>

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

namespace {

// Route exclusion matches the RAW path, deliberately: at before_handle time Crow
// has not resolved a route yet, and resolving one here would add a third O(N)
// scan per request - taxing /health, which resolves nothing today. All the
// default exclusions are literals, so raw-path matching is exact for them.
// The query string is ignored so `/health?x=1` cannot smuggle a probe into the
// trace.
// An OpenTelemetry span's parent is fixed at creation, but SEP-414 puts the
// agent's traceparent inside the JSON-RPC BODY - which the MCP handler does not
// parse until well after before_handle. Waiting for it would mean creating the
// span late and losing every middleware rejection, which is the whole reason the
// span lives in the first middleware.
//
// So peek for just that one key here. The cost falls only on POSTs to the MCP
// endpoint, and the handler re-parses the body anyway today.
SpanContextIds metaTraceContextFromBody(const crow::request& req) {
    // Bounded at 64 KiB, not 1 MiB. This parse happens in the FIRST middleware -
    // before auth and before the rate limiter - so an unauthenticated client can
    // drive it. A conforming MCP request carrying a traceparent is far below this;
    // anything larger is not worth parsing ahead of the rate limiter.
    static constexpr std::size_t kMaxPeekBytes = 64 * 1024;
    if (req.body.empty() || req.body.size() > kMaxPeekBytes) {
        return {};
    }
    const auto doc = crow::json::load(req.body);
    if (!doc || !doc.has("params")) {
        return {};
    }
    const auto& params = doc["params"];
    if (params.t() != crow::json::type::Object || !params.has("_meta")) {
        return {};
    }
    const auto& meta = params["_meta"];
    if (meta.t() != crow::json::type::Object || !meta.has(sep414::kTraceparent)) {
        return {};
    }
    if (meta[sep414::kTraceparent].t() != crow::json::type::String) {
        return {};
    }
    const auto raw = meta[sep414::kTraceparent].s();
    if (raw.size() > 256) {
        return {};   // bounded before copying
    }
    return parseTraceparent(std::string(raw));
}

bool isMcpEndpoint(std::string_view raw_path) {
    // Exact match on the path, ignoring any query string: a prefix match would
    // also catch /mcp/jsonrpc-something-else.
    const auto query = raw_path.find('?');
    const auto path = query == std::string_view::npos ? raw_path : raw_path.substr(0, query);
    return path == "/mcp/jsonrpc";
}

bool isExcludedRoute(const std::vector<std::string>& excluded, std::string_view raw_path) {
    const auto query = raw_path.find('?');
    const auto path = query == std::string_view::npos ? raw_path : raw_path.substr(0, query);
    for (const auto& candidate : excluded) {
        if (path == candidate) { return true; }
    }
    return false;
}

const char* errorTypeFor(int status_code) {
    // Enumerated, never a free-form message: an exception string is the most
    // reliable way to leak customer data into a trace.
    switch (status_code) {
        case 400: return "bad_request";
        case 401: return "unauthenticated";
        case 403: return "permission_denied";
        case 404: return "not_found";
        case 405: return "method_not_allowed";
        case 413: return "payload_too_large";
        case 429: return "rate_limited";
        case 503: return "service_unavailable";
        default:  break;
    }
    if (status_code >= 500) { return "internal_error"; }
    if (status_code >= 400) { return "client_error"; }
    return nullptr;
}

}  // namespace

void RequestContextMiddleware::setConfigManager(std::shared_ptr<ConfigManager> config_manager) {
    config_manager_ = std::move(config_manager);
    // Resolve the logger once at bootstrap. getAuditLogger() initialises lazily
    // and is not synchronised, so resolving it here - on the single-threaded
    // startup path - keeps every Crow worker off that race, and keeps the hot
    // path free of shared_ptr refcount traffic when audit is disabled.
    audit_logger_ = config_manager_ ? config_manager_->getAuditLogger() : nullptr;
    if (config_manager_) {
        const auto& tracing = config_manager_->getTracingConfig();
        excluded_routes_ = tracing.exclude_routes;
        tracing_configured_ = tracing.enabled;
        openinference_ = tracing.openinference;
        capture_policy_ = &config_manager_->getCapturePolicy();
    }
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
    // SEP-414 precedence: params._meta beats the HTTP header. Over a proxy or
    // gateway the HTTP hop may be the gateway's own span while _meta carries the
    // agent's, so preferring _meta keeps flAPI attached to the trace the user
    // actually cares about.
    // Parsed whenever the request is an MCP call, NOT only when tracing is
    // enabled. Correlation is the P0 feature that works with no exporter at all:
    // the ids flow into the audit log and the application log so an agent trace
    // can be joined to a flAPI audit line. Gating this on tracing.enabled broke
    // exactly that, and the SEP-414 correlation tests caught it.
    //
    // The cost is bounded and narrow: only POSTs to /mcp/jsonrpc, only bodies
    // under 64 KiB.
    const auto meta_ids = isMcpEndpoint(ctx.rc.raw_path) ? metaTraceContextFromBody(req)
                                                         : SpanContextIds{};
    const auto resolved = resolvePrecedence(meta_ids, header_ids);
    if (resolved.ids.valid()) {
        ctx.rc.setTraceId(resolved.ids.trace_id);
        ctx.rc.setSpanId(resolved.ids.span_id);
        ctx.rc.sampled = resolved.ids.sampled();
    }
    ctx.rc.trace_context_source = contextSourceName(resolved.source);

    RequestContextScope::activate(&ctx.rc);

    // The HTTP SERVER span. Started here - in the FIRST middleware - because Crow
    // runs before_handle in declaration order, so this is the only position that
    // brackets rate limiting and auth and therefore sees the 401/403/429
    // rejections operators most often ask about. A span started in the request
    // handler would miss every one of them, plus every static route and every
    // 404, since those never reach handleDynamicRequest at all.
    //
    // The name is provisional: http.route is not known until the handler resolves
    // it, so the span opens as "METHOD" and is renamed at completion. Delaying the
    // span to learn the route would defeat the entire point.
    if (!isExcludedRoute(excluded_routes_, ctx.rc.raw_path)) {
        ctx.span = Tracing().startServerSpan(ctx.rc.http_method, resolved);
        if (ctx.span) {
            const auto ids = ctx.span.ids();
            if (ids.valid()) {
                // Stamp back, so the audit line and every log line emitted during
                // this request carry the SAME ids as the span.
                ctx.rc.setTraceId(ids.trace_id);
                ctx.rc.setSpanId(ids.span_id);
                ctx.rc.sampled = ids.sampled();
            }
            ctx.span.setAttr(semconv::http::kRequestMethod, ctx.rc.http_method);
            ctx.span.setAttr(semconv::net::kProtocolName, "http");
            ctx.span.setAttr(semconv::flapix::kHasQuery,
                             ctx.rc.raw_path.find('?') != std::string_view::npos);
            if (const auto ua = req.get_header_value("User-Agent"); !ua.empty()) {
                ctx.span.setAttr(semconv::kUserAgent, std::string_view(ua).substr(0, 256));
            }
            if (ctx.rc.trace_context_source != nullptr) {
                ctx.span.setAttr(semconv::flapix::kContextSource, ctx.rc.trace_context_source);
            }
        }
    }

    // Always server-minted; an inbound X-Request-Id is never honoured, or a
    // client could forge collisions and inject into log lines.
    //
    // Wrapped because instrumentation must never fail a request: set_header
    // allocates, and an exception escaping before_handle would both break the
    // request and leave the ambient pointer set on a pooled worker.
    try {
        res.set_header("X-Request-Id", std::string(ctx.rc.requestIdView()));
    } catch (...) {
        instrumentation_failures_.fetch_add(1, std::memory_order_relaxed);
    }
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

    // F1: only a request that actually produced an exportable span should pay
    // the blocking budget. Captured before end() because the scope is released
    // there.
    bool had_recorded_span = false;

    try {
        if (ctx.span) {
            had_recorded_span = ctx.span.recording();
            // http.route is only known now. It is ALWAYS a template or a fixed
            // literal - never a filled path (NFR-5) - and unmatched paths all
            // collapse to one bucket so a scanner cannot mint a thousand span
            // names and metric series.
            // MCP requests are not endpoint-resolved, so route_template is
            // never filled for them. Reporting "<unmatched>" was bounded but
            // uninformative: it put every agent call in the same bucket as a
            // scanner's 404s. The transport path is a fixed literal, so it is
            // safe as a label and it is what a consumer filtering by route
            // actually wants.
            // MCP first: its requests are never endpoint-resolved, so
            // route_template still holds its default. Reporting "<unmatched>"
            // was bounded but uninformative - it put every agent call in the
            // same bucket as a scanner's 404s. The transport path is a fixed
            // literal, so it is safe as a label and it is what a consumer
            // filtering by route actually wants.
            //
            // route_template DEFAULTS to "<unmatched>" rather than being empty,
            // so testing it for emptiness here would be dead code.
            const std::string_view route =
                isMcpEndpoint(ctx.rc.raw_path) ? std::string_view(semconv::kMcpRoute)
                                               : ctx.rc.route_template;
            // An MCP call is ONE span carrying both attribute sets, named by the
            // MCP convention - "tools/call query_customers" is what a consumer of
            // the trace looks for, not "POST /mcp/jsonrpc".
            //
            // This is correct only because flAPI's transport is strictly one
            // JSON-RPC call per HTTP request: there is no batch handling, and the
            // legacy GET/SSE stream is deliberately not implemented (GET returns
            // 405). If EITHER changes, this collapses and the model must become an
            // HTTP SERVER parent with one MCP SERVER child per call.
            std::string span_name;
            if (!ctx.rc.mcp_method.empty()) {
                span_name = ctx.rc.mcp_method;
                if (!ctx.rc.mcp_tool.empty()) {
                    span_name += ' ';
                    span_name += ctx.rc.mcp_tool;
                }
            } else {
                span_name = ctx.rc.http_method;
                span_name += ' ';
                span_name.append(route);
            }
            ctx.span.updateName(span_name);

            ctx.span.setAttr(semconv::http::kRoute, route);
            ctx.span.setAttr(semconv::url::kPath, route);   // template only
            ctx.span.setAttr(semconv::http::kResponseStatus,
                             static_cast<std::int64_t>(ctx.rc.status_code));
            ctx.span.setAttr(semconv::flapix::kAuthKind, ctx.rc.auth_kind);
            ctx.span.setAttr(semconv::http::kResponseBodySize,
                             static_cast<std::int64_t>(res.body.size()));
            if (ctx.rc.row_count >= 0) {
                ctx.span.setAttr(semconv::db::kReturnedRows, ctx.rc.row_count);
            }
            if (!ctx.rc.mcp_method.empty()) {
                ctx.span.setAttr(semconv::mcp::kMethodName, ctx.rc.mcp_method);
            }
            if (!ctx.rc.mcp_tool.empty()) {
                ctx.span.setAttr(semconv::genai::kToolName, ctx.rc.mcp_tool);
                ctx.span.setAttr(semconv::genai::kOperationName, semconv::genai::kExecuteTool);
            }
            // --- Payload tier ------------------------------------------------
            //
            // Values are captured ONLY here, and only when the effective tier is
            // payload. Everything above this point is structure and shape.
            //
            // Note what is still excluded even here: the filled path and query
            // string (a query string on a data API is by definition a filter over
            // customer data), and every credential-shaped key - both enforced by
            // CapturePolicy rather than by remembering to omit them.
            if (capture_policy_ != nullptr
                && capture_policy_->capturesValues(ctx.rc.endpoint_capture)
                && !ctx.rc.audit_params.empty()) {
                // Built through crow::json, never by concatenation. A value
                // containing a quote or a backslash would otherwise forge
                // fields into an attribute we label application/json, and
                // OpenInference consumers parse it.
                //
                // Bounded twice: each value by max_value_bytes in
                // redactAndClamp, and the object as a whole by the two limits
                // below. Without the second bound a wide endpoint can hold
                // fields x 8 KiB per span, x2 with the overlay, across a queue
                // of max_queue_size spans.
                constexpr std::size_t kMaxPayloadFields = 64;
                const std::size_t total_budget = capture_policy_->maxValueBytes() * 8;

                crow::json::wvalue payload = crow::json::wvalue::object();
                std::size_t used = 0;
                std::size_t taken = 0;
                bool truncated = false;
                for (const auto& [key, value] : ctx.rc.audit_params) {
                    if (taken >= kMaxPayloadFields || used >= total_budget) {
                        truncated = true;
                        break;
                    }
                    // Redact FIRST, clamp SECOND - clamping first can truncate
                    // mid-value and leave a partial secret behind.
                    std::string clamped = capture_policy_->redactAndClamp(key, value);
                    used += key.size() + clamped.size();
                    payload[key] = std::move(clamped);
                    ++taken;
                }
                if (truncated) {
                    payload["flapi.truncated"] = true;
                }
                const std::string rendered = payload.dump();
                ctx.span.setAttr("gen_ai.tool.call.arguments", rendered);

                if (openinference_) {
                    ctx.span.setAttr("input.value", rendered);
                    ctx.span.setAttr("input.mime_type", "application/json");
                }
            }

            // --- OpenInference overlay ---------------------------------------
            //
            // Attributes on the SAME span: no extra spans, no second exporter. A
            // REST request is deliberately NOT marked as a CHAIN - that would
            // mis-model flAPI as an agent rather than as a tool an agent calls.
            if (openinference_ && !ctx.rc.mcp_tool.empty()) {
                ctx.span.setAttr("openinference.span.kind", "TOOL");
                ctx.span.setAttr("tool.name", ctx.rc.mcp_tool);
                if (!ctx.rc.mcp_session_id.empty()) {
                    ctx.span.setAttr("session.id", ctx.rc.mcp_session_id);
                }
            }

            if (const char* error_type = errorTypeFor(ctx.rc.status_code)) {
                ctx.span.setError(error_type);
            }
            ctx.span.end();

            // Hand the trace id back to the caller.
            //
            // The operator-facing payoff of the whole epic: a user hitting an
            // error quotes ONE id and support goes straight to the trace and the
            // audit line, instead of reconstructing the request from two unjoined
            // logs. Exposed on every traced response, not only errors, because an
            // error is not the only reason to ask what happened.
            if (ctx.rc.hasTrace()) {
                res.set_header("X-Trace-Id", std::string(ctx.rc.traceIdView()));
            }
        }
        emitAuditLine(audit_logger_, ctx.rc);
    } catch (...) {
        instrumentation_failures_.fetch_add(1, std::memory_order_relaxed);
    }

    // --- Blocking export, for request-billed scale-to-zero platforms --------
    //
    // Opt-in only (tracing.flush.blocking_timeout_ms), and deliberately LAST:
    // the span is already ended above, so this flush carries it.
    //
    // This runs BEFORE the response reaches the socket. Crow's
    // complete_request() calls the after-handlers, then compresses, then writes
    // (crow/http_connection.h:218-254), so the cost is in the caller's latency
    // by construction. That is not an oversight - on a platform that throttles
    // CPU the instant the response is sent, before-the-response is the ONLY
    // window where the export is guaranteed to be scheduled.
    //
    // One ForceFlush per request, not per span: the processor is still a
    // BatchSpanProcessor, so a request's whole span tree leaves in a single
    // round trip, and any spans left buffered by concurrent or background work
    // are swept out with it.
    //
    // Gated on this request having RECORDED a span. finish() is reached for
    // excluded routes too - exclusion skips span creation, not the context - so
    // an ungated flush would make /health/live wait the full budget against a
    // wedged collector. On a platform with a tight liveness timeout that is a
    // restart of a healthy instance, caused entirely by instrumentation. It
    // would also turn spans_flush_timeouts into a count of probes rather than
    // of lost traces.
    if (had_recorded_span) {
        if (const auto budget = Tracing().blockingFlush()) {
            try {
                if (!Tracing().forceFlush(*budget)) {
                    FlapiTracing::noteFlushTimeout();
                }
            } catch (...) {
                instrumentation_failures_.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
}


}  // namespace flapi
