#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>

#include "handler_pool.hpp"

using namespace flapi;
using namespace std::chrono_literals;

TEST_CASE("submitted work runs", "[handler_pool]") {
    HandlerPool pool(2, 16);
    std::atomic<int> ran{0};
    for (int i = 0; i < 8; ++i) {
        REQUIRE(pool.submit([&ran] { ++ran; }));
    }
    pool.shutdown();
    REQUIRE(ran.load() == 8);
}

TEST_CASE("a full queue refuses work rather than growing", "[handler_pool]") {
    // The property that keeps a slow backend from turning into unbounded
    // memory and unbounded latency. A caller that is refused must answer the
    // request itself; silently queueing it would mean eventually serving a
    // client that stopped waiting long ago.
    HandlerPool pool(1, 2);
    std::atomic<bool> release{false};
    std::atomic<bool> started{false};

    REQUIRE(pool.submit([&] {
        started = true;
        while (!release.load()) {
            std::this_thread::sleep_for(1ms);
        }
    }));
    while (!started.load()) {
        std::this_thread::sleep_for(1ms);
    }

    // The single worker is busy, so these fill the queue.
    REQUIRE(pool.submit([] {}));
    REQUIRE(pool.submit([] {}));
    REQUIRE_FALSE(pool.submit([] {}));   // full
    REQUIRE(pool.rejected() == 1);

    release = true;
    pool.shutdown();
}

TEST_CASE("queued work is drained on shutdown, not dropped", "[handler_pool]") {
    // Each queued job owns a connection waiting for a response. Dropping one
    // leaves that client hanging until its own timeout.
    HandlerPool pool(1, 64);
    std::atomic<int> ran{0};
    std::atomic<bool> release{false};

    REQUIRE(pool.submit([&] {
        while (!release.load()) {
            std::this_thread::sleep_for(1ms);
        }
        ++ran;
    }));
    for (int i = 0; i < 10; ++i) {
        REQUIRE(pool.submit([&ran] { ++ran; }));
    }

    release = true;
    pool.shutdown();
    REQUIRE(ran.load() == 11);
}

TEST_CASE("submitting after shutdown is refused", "[handler_pool]") {
    HandlerPool pool(1, 4);
    pool.shutdown();
    REQUIRE_FALSE(pool.submit([] {}));
}

TEST_CASE("a throwing job does not take the worker with it", "[handler_pool]") {
    HandlerPool pool(1, 8);
    std::atomic<int> ran{0};
    REQUIRE(pool.submit([] { throw std::runtime_error("boom"); }));
    REQUIRE(pool.submit([&ran] { ++ran; }));
    pool.shutdown();
    REQUIRE(ran.load() == 1);
}
