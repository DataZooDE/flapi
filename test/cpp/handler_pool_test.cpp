#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <future>
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

TEST_CASE("shutdown abandons the queue once the drain budget expires",
          "[handler_pool][shutdown]") {
    // The budget bounds the QUEUE, not a running job - a worker inside a
    // query cannot be interrupted. What it must guarantee is that one slow
    // job does not drag every queued job along with it into a shutdown the
    // platform is timing.
    HandlerPool pool(1, 32);

    std::atomic<int> ran{0};
    std::promise<void> first_started;
    auto first_started_future = first_started.get_future();
    std::atomic<bool> release{false};

    REQUIRE(pool.submit([&] {
        first_started.set_value();
        while (!release.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ++ran;
    }));
    first_started_future.wait();

    for (int i = 0; i < 5; ++i) {
        REQUIRE(pool.submit([&] { ++ran; }));
    }

    // A zero budget: the deadline is already past when shutdown() records it,
    // so the worker cannot fail to observe it however the scheduler behaves.
    // A 10ms budget raced - if the worker reached its next queued job before
    // the 10ms elapsed it ran one, and the assertion below flaked.
    std::thread stopper([&] { pool.shutdown(std::chrono::milliseconds(0)); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    release.store(true);
    stopper.join();

    // The running job finished; the five queued behind it were abandoned.
    REQUIRE(ran.load() == 1);
}

TEST_CASE("a second shutdown waits for the first rather than returning early",
          "[handler_pool][shutdown]") {
    // shutdown() used to see `stopping_` already true and return BEFORE the
    // join loop, so a second caller believed a pool had drained that had
    // joined nothing. ~HandlerPool during static destruction took that same
    // early return, which is the path the original SIGSEGV came from.
    HandlerPool pool(2, 8);

    std::promise<void> started;
    auto started_future = started.get_future();
    std::atomic<bool> release{false};
    std::atomic<bool> job_finished{false};

    REQUIRE(pool.submit([&] {
        started.set_value();
        while (!release.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        job_finished.store(true);
    }));
    started_future.wait();

    // BOTH callers are instrumented, and the first is started first and
    // given time to take the shutdown lock. Instrumenting only the second
    // meant the test could not tell which caller had returned early, so it
    // detected the regression only when the threads happened to interleave
    // the right way.
    std::atomic<bool> first_returned{false};
    std::atomic<bool> second_returned{false};

    std::thread first([&] {
        pool.shutdown(std::chrono::seconds(5));
        first_returned.store(true);
    });
    // Let the first caller get inside shutdown() before the second arrives,
    // so the second is unambiguously the one that must WAIT.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::thread second([&] {
        pool.shutdown(std::chrono::seconds(5));
        second_returned.store(true);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    // The job is still running, so NEITHER caller may have returned. The
    // early-return regression shows up here as second_returned == true.
    REQUIRE_FALSE(first_returned.load());
    REQUIRE_FALSE(second_returned.load());

    release.store(true);
    first.join();
    second.join();

    REQUIRE(job_finished.load());
    REQUIRE(first_returned.load());
    REQUIRE(second_returned.load());
}
