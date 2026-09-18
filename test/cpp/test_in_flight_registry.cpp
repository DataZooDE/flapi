#include <catch2/catch_all.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "in_flight_registry.hpp"

using namespace flapi;
using namespace std::chrono_literals;

namespace {
// Time is injected rather than slept, so these tests are exact instead of
// merely probable.
std::chrono::steady_clock::time_point at(int ms) {
    static const auto base = std::chrono::steady_clock::now();
    return base + std::chrono::milliseconds(ms);
}
}  // namespace

TEST_CASE("an idle server reports no in-flight work", "[in_flight]") {
    InFlightRegistry::resetForTesting();
    REQUIRE(InFlightRegistry::inFlight() == 0);
    REQUIRE(InFlightRegistry::oldestAge(at(1000)) == 0ms);
}

TEST_CASE("the oldest in-flight request's age is reported", "[in_flight]") {
    InFlightRegistry::resetForTesting();
    const auto slot = InFlightRegistry::begin(at(0));
    REQUIRE(InFlightRegistry::inFlight() == 1);
    REQUIRE(InFlightRegistry::oldestAge(at(250)) == 250ms);
    InFlightRegistry::end(slot);
    REQUIRE(InFlightRegistry::inFlight() == 0);
    REQUIRE(InFlightRegistry::oldestAge(at(250)) == 0ms);
}

TEST_CASE("a finished request stops counting even when others continue",
          "[in_flight]") {
    // The failure this guards against: reporting a stale age forever because a
    // slot was never cleared. A health check that latches on is as useless as
    // one that never fires.
    InFlightRegistry::resetForTesting();

    std::atomic<bool> release{false};
    std::atomic<int> started{0};
    std::vector<std::thread> workers;
    for (int i = 0; i < 2; ++i) {
        workers.emplace_back([&, i] {
            const auto slot = InFlightRegistry::begin(at(i * 100));
            ++started;
            while (!release.load()) {
                std::this_thread::sleep_for(1ms);
            }
            InFlightRegistry::end(slot);
        });
    }
    while (started.load() < 2) {
        std::this_thread::sleep_for(1ms);
    }

    REQUIRE(InFlightRegistry::inFlight() == 2);
    // The OLDEST of the two, not the most recent.
    REQUIRE(InFlightRegistry::oldestAge(at(500)) == 500ms);

    release.store(true);
    for (auto& w : workers) {
        w.join();
    }
    REQUIRE(InFlightRegistry::inFlight() == 0);
    REQUIRE(InFlightRegistry::oldestAge(at(500)) == 0ms);
}

TEST_CASE("end() without begin() is harmless", "[in_flight]") {
    // finish() runs on paths where before_handle never did, so releasing an
    // unclaimed slot is a real call pattern rather than a hypothetical.
    InFlightRegistry::resetForTesting();
    InFlightRegistry::end(InFlightRegistry::kNone);
    REQUIRE(InFlightRegistry::inFlight() == 0);
    REQUIRE(InFlightRegistry::oldestAge(at(10)) == 0ms);
}

TEST_CASE("concurrent requests get distinct slots", "[in_flight]") {
    // If two requests shared a slot the count and the age would both be wrong,
    // in whichever direction the race fell.
    InFlightRegistry::resetForTesting();

    constexpr int kThreads = 8;
    std::atomic<int> ready{0};
    std::atomic<bool> release{false};
    std::vector<std::thread> workers;
    for (int i = 0; i < kThreads; ++i) {
        workers.emplace_back([&] {
            const auto slot = InFlightRegistry::begin(at(0));
            ++ready;
            while (!release.load()) {
                std::this_thread::sleep_for(1ms);
            }
            InFlightRegistry::end(slot);
        });
    }
    while (ready.load() < kThreads) {
        std::this_thread::sleep_for(1ms);
    }

    REQUIRE(InFlightRegistry::inFlight() == static_cast<std::size_t>(kThreads));

    release.store(true);
    for (auto& w : workers) {
        w.join();
    }
    REQUIRE(InFlightRegistry::inFlight() == 0);
}


TEST_CASE("a slot released from another thread still frees", "[in_flight]") {
    // The reason slots are handles rather than thread-locals. Crow completes a
    // response on the handling thread today, but a deferred completion would
    // otherwise latch the slot forever and wedge readiness with a permanent
    // false stall - a health check that takes the service down on its own.
    InFlightRegistry::resetForTesting();

    const auto slot = InFlightRegistry::begin(at(0));
    REQUIRE(InFlightRegistry::inFlight() == 1);

    std::thread other([slot] { InFlightRegistry::end(slot); });
    other.join();

    REQUIRE(InFlightRegistry::inFlight() == 0);
    REQUIRE(InFlightRegistry::oldestAge(at(999)) == 0ms);
}
