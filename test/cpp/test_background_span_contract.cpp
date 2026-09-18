// The background-thread root-span contract (plan section 3.2, crew finding F5
// from the P0 review).
//
// OpenTelemetry's active-span stack is THREAD-LOCAL. That is what lets the inner
// pipeline attach children with no signature changes - but only for the
// synchronous request stack. MCPTaskManager workers, the heartbeat worker, cache
// refresh and warmup all run off-request, where the ambient context is either
// absent (an orphan span) or, worse, STALE - attached to a previous, unrelated
// request that happened to run on the same pooled thread.
//
// The rule is therefore: background threads start ROOT spans and never implicit
// children. This pins it.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>
#include <vector>

#include "request_context.hpp"

using namespace flapi;

TEST_CASE("a fresh thread has no ambient request context", "[background][tracing]") {
    RequestContext rc;
    RequestContext::mintRequestId(rc.request_id);
    RequestContextScope scope(&rc);
    REQUIRE(RequestContextScope::current() == &rc);

    std::atomic<bool> worker_saw_context{true};
    std::thread worker([&] {
        // A worker must NOT inherit the submitting request's context. If it did,
        // background work would be attributed to whatever request happened to
        // start it, and its spans would attach to a parent that has already ended.
        worker_saw_context.store(RequestContextScope::current() != nullptr);
    });
    worker.join();

    REQUIRE_FALSE(worker_saw_context.load());
    REQUIRE(RequestContextScope::current() == &rc);   // unchanged on this thread
}

TEST_CASE("a pooled thread does not leak context between units of work",
          "[background][tracing]") {
    // The stale-context hazard, stated directly: a thread that served a request
    // and is then reused for background work must not still be carrying it.
    std::atomic<int> leaks{0};

    std::thread pooled([&] {
        {
            RequestContext first;
            RequestContext::mintRequestId(first.request_id);
            RequestContextScope scope(&first);
            REQUIRE(RequestContextScope::current() != nullptr);
        }
        // The scope has ended: this thread is now doing "background" work.
        if (RequestContextScope::current() != nullptr) {
            leaks.fetch_add(1);
        }

        {
            RequestContext second;
            RequestContext::mintRequestId(second.request_id);
            RequestContextScope scope(&second);
            if (RequestContextScope::current()->requestIdView() != second.requestIdView()) {
                leaks.fetch_add(1);
            }
        }
        if (RequestContextScope::current() != nullptr) {
            leaks.fetch_add(1);
        }
    });
    pooled.join();

    REQUIRE(leaks.load() == 0);
}

TEST_CASE("many threads keep their contexts independent", "[background][tracing]") {
    constexpr int kThreads = 8;
    std::atomic<int> mismatches{0};
    std::vector<std::thread> threads;

    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&] {
            RequestContext rc;
            RequestContext::mintRequestId(rc.request_id);
            RequestContextScope scope(&rc);
            for (int j = 0; j < 500; ++j) {
                if (RequestContextScope::current() != &rc) {
                    mismatches.fetch_add(1);
                    break;
                }
            }
        });
    }
    for (auto& t : threads) { t.join(); }

    REQUIRE(mismatches.load() == 0);
    REQUIRE(RequestContextScope::current() == nullptr);
}
