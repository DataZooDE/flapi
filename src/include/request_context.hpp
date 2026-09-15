#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace flapi {

// One object per in-flight request: the identity and the single clock that the
// audit log, the application log and the trace all share.
//
// Before this existed, flAPI timed overlapping operations with three separate
// clocks (api_server.cpp, mcp_tool_handler.cpp twice) and minted a request id
// that nothing outside the audit logger could see, so an audit line, a log line
// and a telemetry event could not be joined.
//
// Allocation matters here: this is constructed on EVERY request on EVERY route,
// including Kubernetes health probes, whether or not tracing is enabled. Hence
// fixed-width character arrays for the ids (a 32-char W3C trace id exceeds
// libstdc++'s 15-byte small-string buffer, so std::string would heap-allocate on
// every traced request - measured in test_alloc_budget.cpp), const char* for the
// closed enumerations, and a flat vector for audit params that is only populated
// when audit is actually enabled.
struct RequestContext {
    // ---- identity ---------------------------------------------------------
    std::array<char, 20> request_id{};   // "req-" + 16 hex; ALWAYS server-minted
    std::array<char, 32> trace_id{};     // W3C; all-zero means "no trace"
    std::array<char, 16> span_id{};
    bool sampled = false;

    // ---- the single clock -------------------------------------------------
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    std::int64_t elapsedMs() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - t0).count();
    }

    // ---- request facts, filled progressively ------------------------------
    // raw_path is for logs and exclude-matching only and is NEVER exported: a
    // filled path on a data API is a filter over customer data.
    // string_view into crow::request::url, which outlives the middleware context.
    // A copy here would allocate on every request including health probes - the
    // regression NFR-1 exists to prevent.
    std::string_view raw_path;
    // Points either at a static literal ("<unmatched>") or into the pinned
    // endpoint snapshot; never owns.
    std::string_view route_template{"<unmatched>"};
    // Backing store for route_template when it must outlive the pinned endpoint
    // snapshot. Only ever written once a route actually matched, so the common
    // unmatched/static-route case stays allocation-free.
    std::string route_template_storage;
    const char* http_method = "";        // crow::method_name() - static storage
    const char* auth_kind = "none";      // bounded enumeration
    std::string principal = "anonymous";
    std::string mcp_method, mcp_tool, mcp_session_id;
    std::int64_t row_count = -1;
    int status_code = 0;
    std::vector<std::pair<std::string, std::string>> audit_params;  // already redacted

    // Set by a richer emitter (the MCP tool handler) to tell the HTTP middleware
    // not to also write a line. An MCP tools/call is ONE operation: auditing both
    // the HTTP POST and the tool call would double audit volume and report the
    // same work twice under different names. Mirrors the single-span contract the
    // trace model uses for the same reason.
    bool audit_suppressed = false;

    // Where the trace context came from: "none", "header", "meta", or
    // "meta_over_header" when _meta and the HTTP header disagreed. Recorded so a
    // surprising parent is diagnosable rather than mysterious. Static storage.
    const char* trace_context_source = "none";

    // ---- accessors --------------------------------------------------------
    std::string_view requestIdView() const { return viewOf(request_id); }
    std::string_view traceIdView() const { return viewOf(trace_id); }
    std::string_view spanIdView() const { return viewOf(span_id); }
    bool hasTrace() const { return trace_id[0] != '\0'; }

    void setTraceId(std::string_view v) { assign(trace_id, v); }
    void setSpanId(std::string_view v) { assign(span_id, v); }

    static void mintRequestId(std::array<char, 20>& out);

private:
    template <std::size_t N>
    static std::string_view viewOf(const std::array<char, N>& a) {
        return a[0] == '\0' ? std::string_view{} : std::string_view(a.data(), N);
    }
    template <std::size_t N>
    static void assign(std::array<char, N>& a, std::string_view v) {
        a.fill('\0');
        const std::size_t n = v.size() < N ? v.size() : N;
        for (std::size_t i = 0; i < n; ++i) { a[i] = v[i]; }
    }
};

// Makes the current request's context ambient on this thread.
//
// The scope OWNS the clearing, deliberately, rather than leaving it to a Crow
// after_handle: Crow does not call after_handle when a middleware completes the
// response inside before_handle (crow/http_connection.h:207, which is what
// AuthMiddleware's 401 paths do), and a handler can throw. Both unwind through
// this destructor. Crow's worker threads are pooled, so a context left behind
// would stamp one request's identity onto the next.
//
// Holds a raw pointer, never a shared_ptr: the context is owned by value in the
// middleware's per-request context, so there is nothing to keep alive and no
// refcount atomics on the hot path.
class RequestContextScope {
public:
    explicit RequestContextScope(RequestContext* rc) noexcept;
    ~RequestContextScope() noexcept;

    RequestContextScope(const RequestContextScope&) = delete;
    RequestContextScope& operator=(const RequestContextScope&) = delete;
    RequestContextScope(RequestContextScope&&) = delete;
    RequestContextScope& operator=(RequestContextScope&&) = delete;

    // nullptr when no request is in flight on this thread.
    static RequestContext* current() noexcept;

    // Explicit activation, for the Crow middleware only.
    //
    // RAII cannot be used across Crow's middleware boundary: before_handle and
    // after_handle are separate calls, and the middleware's context object lives
    // on the CONNECTION, not the request - it is reset at the start of the next
    // request on that connection (crow/http_connection.h:143). A scope object
    // stored there would run its destructor at an arbitrary later time, possibly
    // on another thread, and restore a stale pointer over an unrelated request's
    // context.
    //
    // Pairing is instead guaranteed by Crow's own control flow:
    //   - the handler ran        -> after_handle runs        -> clear()
    //   - a middleware ended it  -> explicit finish          -> clear()
    //   - unmatched route (404)  -> after_handle runs (:117) -> clear()
    // A handler that throws still gets after_handle, because the flag is set
    // before the handler is invoked.
    static void activate(RequestContext* rc) noexcept;
    static void clear() noexcept;

private:
    RequestContext* previous_ = nullptr;
};

}  // namespace flapi
