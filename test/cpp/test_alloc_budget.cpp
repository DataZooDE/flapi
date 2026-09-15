// Issue 0c - the proxy gate.
//
// The wall-clock load harness cannot resolve a per-request cost below roughly
// tens of microseconds: on the cheap routes the most stable statistic (median
// TTFB) still varies ~7% run-to-run, and a 200us request would have to grow by
// ~14us to register. The epic's NFR-1 claim - "disabled tracing costs nothing
// measurable" - is therefore carried HERE, by counting allocations, which is
// deterministic and machine-independent.
//
// The counter is process-wide and these tests are not thread-safe against other
// allocating tests running concurrently; Catch2 runs single-threaded by default,
// which is what makes this sound.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>

#include "alloc_counter.hpp"

using flapi::test::AllocCounter;
using flapi::test::doNotOptimise;

TEST_CASE("alloc counter observes allocations", "[perf][alloc]") {
    // Guards the instrument itself: a counter that silently stops counting - or an
    // optimiser that deletes the allocation - would make every budget below pass
    // trivially. This test failing is the signal that the gate is not measuring.
    std::size_t observed = 0;
    {
        AllocCounter c;
        auto* v = new std::vector<int>();
        doNotOptimise(v);
        v->reserve(64);
        doNotOptimise(*v);
        observed = c.count();
        delete v;
    }
    INFO("allocations observed: " << observed);
    REQUIRE(observed >= 2);
}

TEST_CASE("alloc counter is quiet when nothing allocates", "[perf][alloc]") {
    AllocCounter c;
    volatile int x = 0;
    for (int i = 0; i < 1000; ++i) { x += i; }
    (void)x;
    REQUIRE(c.count() == 0);
}

TEST_CASE("a W3C trace id cannot live in a std::string for free", "[perf][alloc]") {
    // The measurement behind RequestContext storing trace/span ids as std::array
    // rather than std::string. libstdc++'s small-string buffer is 15 bytes; a W3C
    // trace id is 32 hex characters and a span id 16, so std::string allocates on
    // EVERY traced request for values whose length is fixed by the spec.
    std::size_t short_allocs = 0, trace_allocs = 0, span_allocs = 0;
    {
        AllocCounter c;
        std::string tiny(8, 'a');           // inside SSO
        doNotOptimise(tiny);
        short_allocs = c.count();
    }
    {
        AllocCounter c;
        std::string trace_id(32, 'a');      // a W3C trace id
        doNotOptimise(trace_id);
        trace_allocs = c.count();
    }
    {
        AllocCounter c;
        std::string span_id(16, 'a');       // a W3C span id
        doNotOptimise(span_id);
        span_allocs = c.count();
    }
    INFO("8 chars: " << short_allocs << "  32 chars: " << trace_allocs
         << "  16 chars: " << span_allocs);
    REQUIRE(short_allocs == 0);
    REQUIRE(trace_allocs >= 1);
    REQUIRE(span_allocs >= 1);
}
