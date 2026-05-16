#include "mcp_tool_rate_limiter.hpp"

#include "config_manager.hpp"

namespace flapi {

MCPToolRateLimiter::MCPToolRateLimiter()
    : clock_([]() { return std::chrono::steady_clock::now(); }) {}

MCPToolRateLimiter::MCPToolRateLimiter(Clock clock)
    : clock_(std::move(clock)) {}

std::string MCPToolRateLimiter::keyFor(const std::string& tool_name,
                                       const std::string& principal) {
    return tool_name + "|" + principal;
}

MCPToolRateLimiter::AcquireDecision MCPToolRateLimiter::tryAcquire(
    const std::string& tool_name,
    const std::string& principal,
    const RateLimitConfig& config) {

    if (!config.enabled) {
        return {true, /*remaining=*/0, /*retry_after=*/0};
    }

    const auto key = keyFor(tool_name, principal);
    const auto now = clock_();

    std::lock_guard<std::mutex> guard(mutex_);
    auto& bucket = buckets_[key];

    // Initialise or roll over an expired window.
    if (now >= bucket.reset_time) {
        bucket.remaining = config.max;
        bucket.reset_time = now + std::chrono::seconds(config.interval);
    }

    if (bucket.remaining <= 0) {
        const auto until = std::chrono::duration_cast<std::chrono::seconds>(
            bucket.reset_time - now).count();
        return {false, /*remaining=*/0, /*retry_after=*/std::max<std::int64_t>(1, until)};
    }

    bucket.remaining -= 1;
    return {true, bucket.remaining, /*retry_after=*/0};
}

} // namespace flapi
