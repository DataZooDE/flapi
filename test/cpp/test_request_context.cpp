// Issue 1 - the shared RequestContext.
//
// One object per in-flight request, carrying the identity and the single clock
// that the audit log, the application log and (later) the trace all share. Today
// flAPI has three separate clocks for overlapping operations
// (api_server.cpp:218, mcp_tool_handler.cpp:14, :35) and no request identity that
// anything but the audit log can see.
//
// The lifetime tests below are the important ones: the ambient pointer lives in
// thread-local storage on Crow's pooled worker threads, so a context that is not
// cleared on every exit path would stamp one request's identity onto the next.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <regex>
#include <string>
#include <thread>
#include <vector>

#include "request_context.hpp"

using namespace flapi;

TEST_CASE("request ids are well formed and unique", "[request_context]") {
    RequestContext a, b;
    RequestContext::mintRequestId(a.request_id);
    RequestContext::mintRequestId(b.request_id);

    const std::string sa(a.request_id.data(), a.request_id.size());
    const std::string sb(b.request_id.data(), b.request_id.size());

    REQUIRE(std::regex_match(sa, std::regex("^req-[0-9a-f]{16}$")));
    REQUIRE(sa != sb);
}

TEST_CASE("elapsedMs is monotonic and starts at zero", "[request_context]") {
    RequestContext rc;
    rc.t0 = std::chrono::steady_clock::now();
    REQUIRE(rc.elapsedMs() >= 0);

    const auto first = rc.elapsedMs();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    REQUIRE(rc.elapsedMs() >= first);
    REQUIRE(rc.elapsedMs() >= 4);
}

TEST_CASE("trace ids are empty until set", "[request_context]") {
    RequestContext rc;
    REQUIRE_FALSE(rc.hasTrace());
    REQUIRE(rc.traceIdView().empty());
    REQUIRE(rc.spanIdView().empty());

    rc.setTraceId("4bf92f3577b34da6a3ce929d0e0e4736");
    rc.setSpanId("00f067aa0ba902b7");
    REQUIRE(rc.hasTrace());
    REQUIRE(rc.traceIdView() == "4bf92f3577b34da6a3ce929d0e0e4736");
    REQUIRE(rc.spanIdView() == "00f067aa0ba902b7");
}

TEST_CASE("the ambient context is null outside any scope", "[request_context]") {
    REQUIRE(RequestContextScope::current() == nullptr);
}

TEST_CASE("the ambient context is visible inside its scope", "[request_context]") {
    RequestContext rc;
    {
        RequestContextScope scope(&rc);
        REQUIRE(RequestContextScope::current() == &rc);
    }
    REQUIRE(RequestContextScope::current() == nullptr);
}

TEST_CASE("nested scopes restore the previous context", "[request_context]") {
    RequestContext outer, inner;
    RequestContextScope a(&outer);
    REQUIRE(RequestContextScope::current() == &outer);
    {
        RequestContextScope b(&inner);
        REQUIRE(RequestContextScope::current() == &inner);
    }
    REQUIRE(RequestContextScope::current() == &outer);
}

TEST_CASE("the ambient context does not leak between sequential requests", "[request_context]") {
    // The pooled-worker hazard, stated as a test: request A's identity must not
    // be readable while request B runs on the same thread.
    RequestContext a;
    RequestContext::mintRequestId(a.request_id);
    {
        RequestContextScope scope(&a);
        REQUIRE(RequestContextScope::current() != nullptr);
    }
    REQUIRE(RequestContextScope::current() == nullptr);

    RequestContext b;
    RequestContext::mintRequestId(b.request_id);
    {
        RequestContextScope scope(&b);
        REQUIRE(RequestContextScope::current()->requestIdView() == b.requestIdView());
        REQUIRE(RequestContextScope::current()->requestIdView() != a.requestIdView());
    }
    REQUIRE(RequestContextScope::current() == nullptr);
}

TEST_CASE("the ambient context is cleared when the scope unwinds", "[request_context]") {
    // Crow does not call after_handle when a middleware completes the response in
    // before_handle (http_connection.h:207), and a handler can throw. Both unwind
    // through the scope's destructor, which is why the scope - not after_handle -
    // owns the clearing.
    RequestContext rc;
    try {
        RequestContextScope scope(&rc);
        REQUIRE(RequestContextScope::current() == &rc);
        throw std::runtime_error("handler blew up");
    } catch (const std::runtime_error&) {
        // fall through
    }
    REQUIRE(RequestContextScope::current() == nullptr);
}

TEST_CASE("the ambient context is per-thread", "[request_context]") {
    RequestContext main_ctx;
    RequestContextScope scope(&main_ctx);
    REQUIRE(RequestContextScope::current() == &main_ctx);

    std::atomic<bool> other_saw_null{false};
    std::thread t([&] {
        other_saw_null.store(RequestContextScope::current() == nullptr);
    });
    t.join();

    REQUIRE(other_saw_null.load());
    REQUIRE(RequestContextScope::current() == &main_ctx);
}

TEST_CASE("a default RequestContext does not allocate", "[request_context][perf]") {
    // The consolidation must not regress NFR-1: this object is built on EVERY
    // request on EVERY route, including health probes, whether or not tracing is
    // on. Fixed-width ids and const char* enums are what keep it allocation-free.
    REQUIRE(sizeof(RequestContext) > 0);
    RequestContext rc;
    RequestContext::mintRequestId(rc.request_id);
    REQUIRE(rc.requestIdView().size() == 20);
}
