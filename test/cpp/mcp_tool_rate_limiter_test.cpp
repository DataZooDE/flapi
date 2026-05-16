#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "config_manager.hpp"
#include "mcp_tool_rate_limiter.hpp"

namespace flapi {
namespace test {

namespace {

using namespace std::chrono_literals;
using time_point = std::chrono::steady_clock::time_point;

RateLimitConfig makeLimit(int max, int interval) {
    RateLimitConfig c;
    c.enabled = true;
    c.max = max;
    c.interval = interval;
    return c;
}

class FakeClock {
public:
    explicit FakeClock(time_point start = time_point{})
        : now_(start) {}
    time_point now() const { return now_; }
    void advance(std::chrono::seconds delta) { now_ += delta; }
private:
    time_point now_;
};

} // namespace

TEST_CASE("MCPToolRateLimiter: disabled config always allows",
          "[security][mcp][ratelimit]") {
    FakeClock clk;
    MCPToolRateLimiter limiter([&clk]() { return clk.now(); });
    RateLimitConfig cfg;
    cfg.enabled = false;
    cfg.max = 1;
    cfg.interval = 60;

    for (int i = 0; i < 50; ++i) {
        auto decision = limiter.tryAcquire("tool_a", "principal_a", cfg);
        REQUIRE(decision.allowed);
    }
}

TEST_CASE("MCPToolRateLimiter: max=N allows exactly N calls then denies",
          "[security][mcp][ratelimit]") {
    FakeClock clk;
    MCPToolRateLimiter limiter([&clk]() { return clk.now(); });
    auto cfg = makeLimit(/*max=*/3, /*interval=*/60);

    REQUIRE(limiter.tryAcquire("t", "p", cfg).allowed);
    REQUIRE(limiter.tryAcquire("t", "p", cfg).allowed);
    REQUIRE(limiter.tryAcquire("t", "p", cfg).allowed);
    auto denied = limiter.tryAcquire("t", "p", cfg);
    REQUIRE_FALSE(denied.allowed);
    REQUIRE(denied.retry_after_seconds > 0);
}

TEST_CASE("MCPToolRateLimiter: bucket resets after the interval elapses",
          "[security][mcp][ratelimit]") {
    FakeClock clk;
    MCPToolRateLimiter limiter([&clk]() { return clk.now(); });
    auto cfg = makeLimit(/*max=*/2, /*interval=*/30);

    REQUIRE(limiter.tryAcquire("t", "p", cfg).allowed);
    REQUIRE(limiter.tryAcquire("t", "p", cfg).allowed);
    REQUIRE_FALSE(limiter.tryAcquire("t", "p", cfg).allowed);

    // Step past the reset boundary.
    clk.advance(31s);
    REQUIRE(limiter.tryAcquire("t", "p", cfg).allowed);
    REQUIRE(limiter.tryAcquire("t", "p", cfg).allowed);
    REQUIRE_FALSE(limiter.tryAcquire("t", "p", cfg).allowed);
}

TEST_CASE("MCPToolRateLimiter: two tools have independent buckets",
          "[security][mcp][ratelimit]") {
    // Per-tool rate limiting is the whole point of W2.5 — tool A's
    // traffic must not consume tool B's quota.
    FakeClock clk;
    MCPToolRateLimiter limiter([&clk]() { return clk.now(); });
    auto cfg = makeLimit(/*max=*/1, /*interval=*/60);

    REQUIRE(limiter.tryAcquire("tool_a", "p", cfg).allowed);
    REQUIRE_FALSE(limiter.tryAcquire("tool_a", "p", cfg).allowed);

    // tool_b still has a full bucket.
    REQUIRE(limiter.tryAcquire("tool_b", "p", cfg).allowed);
    REQUIRE_FALSE(limiter.tryAcquire("tool_b", "p", cfg).allowed);
}

TEST_CASE("MCPToolRateLimiter: two principals have independent buckets",
          "[security][mcp][ratelimit]") {
    FakeClock clk;
    MCPToolRateLimiter limiter([&clk]() { return clk.now(); });
    auto cfg = makeLimit(/*max=*/2, /*interval=*/60);

    REQUIRE(limiter.tryAcquire("t", "alice", cfg).allowed);
    REQUIRE(limiter.tryAcquire("t", "alice", cfg).allowed);
    REQUIRE_FALSE(limiter.tryAcquire("t", "alice", cfg).allowed);

    // bob is a different principal on the same tool — full quota.
    REQUIRE(limiter.tryAcquire("t", "bob", cfg).allowed);
    REQUIRE(limiter.tryAcquire("t", "bob", cfg).allowed);
    REQUIRE_FALSE(limiter.tryAcquire("t", "bob", cfg).allowed);
}

TEST_CASE("MCPToolRateLimiter: retry_after equals seconds until the bucket resets",
          "[security][mcp][ratelimit]") {
    FakeClock clk;
    MCPToolRateLimiter limiter([&clk]() { return clk.now(); });
    auto cfg = makeLimit(/*max=*/1, /*interval=*/45);

    REQUIRE(limiter.tryAcquire("t", "p", cfg).allowed);
    clk.advance(10s);
    auto denied = limiter.tryAcquire("t", "p", cfg);
    REQUIRE_FALSE(denied.allowed);
    // Reset at t=45, currently at t=10 → ~35s left.
    REQUIRE(denied.retry_after_seconds >= 34);
    REQUIRE(denied.retry_after_seconds <= 35);
}

TEST_CASE("MCPToolRateLimiter: remaining counts down on each successful acquire",
          "[security][mcp][ratelimit]") {
    FakeClock clk;
    MCPToolRateLimiter limiter([&clk]() { return clk.now(); });
    auto cfg = makeLimit(/*max=*/3, /*interval=*/60);

    auto d1 = limiter.tryAcquire("t", "p", cfg);
    REQUIRE(d1.allowed);
    REQUIRE(d1.remaining == 2);

    auto d2 = limiter.tryAcquire("t", "p", cfg);
    REQUIRE(d2.allowed);
    REQUIRE(d2.remaining == 1);

    auto d3 = limiter.tryAcquire("t", "p", cfg);
    REQUIRE(d3.allowed);
    REQUIRE(d3.remaining == 0);
}

TEST_CASE("MCPToolRateLimiter: concurrent acquires honour the cap exactly",
          "[security][mcp][ratelimit][threading]") {
    // Real-clock test: hammer the limiter from N threads with max=K
    // and verify exactly K acquires succeed.
    MCPToolRateLimiter limiter;  // default clock
    auto cfg = makeLimit(/*max=*/50, /*interval=*/60);

    constexpr int kThreads = 16;
    constexpr int kAttemptsPerThread = 25;
    std::atomic<int> allowed_count{0};
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&]() {
            for (int i = 0; i < kAttemptsPerThread; ++i) {
                if (limiter.tryAcquire("t", "p", cfg).allowed) {
                    allowed_count.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& w : workers) {
        w.join();
    }
    REQUIRE(allowed_count.load() == 50);
}

} // namespace test
} // namespace flapi
