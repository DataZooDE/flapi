// Issue 5 - the call-site interface, and the contract that disabled tracing costs
// nothing.
//
// This file MUST compile and pass in BOTH build modes. FLAPI_WITH_TRACING=OFF
// swaps in an inline-empty twin of SpanScope, and if the two halves are allowed to
// drift the OFF build rots silently - which is exactly what happens to build
// configurations nobody exercises.
//
// The allocation assertions are the real NFR-1 gate. The wall-clock harness
// resolves tens of microseconds at best (test/load/README.md), so "costs nothing
// when disabled" cannot be demonstrated by timing - only by counting.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <string>
#include <utility>

#include "flapi_build_config.hpp"
#include "flapi_tracing.hpp"
#include "trace_scope.hpp"

using namespace flapi;

#include "alloc_counter.hpp"

using flapi::test::AllocCounter;
using flapi::test::doNotOptimise;

TEST_CASE("a default SpanScope is inert and pointer-sized", "[trace_scope]") {
    SpanScope s;
    REQUIRE_FALSE(static_cast<bool>(s));
    REQUIRE(sizeof(SpanScope) == sizeof(void*));
}

TEST_CASE("an inert SpanScope allocates nothing", "[trace_scope][perf]") {
    // The NFR-1 gate. Every instrumented call site constructs one of these on
    // every request; if a disabled span allocates, "zero cost when off" is false.
    AllocCounter c;
    {
        SpanScope s = Tracing().startSpan("flapi.noop", SpanKind::Internal);
        doNotOptimise(s);
        if (s) { s.setAttr("never", "reached"); }
        s.end();
    }
    REQUIRE(c.count() == 0);
}

TEST_CASE("operations on an inert SpanScope are safe no-ops", "[trace_scope]") {
    SpanScope s;
    s.setAttr("key", "value");
    s.setAttr("n", static_cast<std::int64_t>(1));
    s.setAttr("d", 1.5);
    s.setAttr("b", true);
    s.addEvent("event");
    s.setError("some_error");
    s.end();
    s.end();                       // idempotent
    REQUIRE(s.ids().trace_id.empty());
    SUCCEED("no crash and no observable effect");
}

TEST_CASE("SpanScope is movable and the source is left inert", "[trace_scope]") {
    SpanScope a;
    SpanScope b = std::move(a);
    REQUIRE_FALSE(static_cast<bool>(a));
    REQUIRE_FALSE(static_cast<bool>(b));

    SpanScope c;
    c = std::move(b);
    REQUIRE_FALSE(static_cast<bool>(b));
}

TEST_CASE("tracing is inactive until explicitly enabled", "[trace_scope][tracing]") {
    // BR-6: tracing is off until an operator turns it on. A default-constructed
    // process must export nothing and start no threads.
    REQUIRE_FALSE(Tracing().isEnabled());
}

TEST_CASE("the build mode is self-consistent", "[trace_scope]") {
    // Guards against the two halves drifting: with tracing compiled out, the
    // facade must report itself disabled no matter what configuration says.
#if FLAPI_WITH_TRACING
    SUCCEED("built with tracing support");
#else
    REQUIRE_FALSE(Tracing().isEnabled());
    SpanScope s = Tracing().startSpan("anything", SpanKind::Server);
    REQUIRE_FALSE(static_cast<bool>(s));
#endif
}
