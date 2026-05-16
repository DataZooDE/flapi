#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace flapi {

struct RateLimitConfig;

// W2.5: Per-tool rate limiter for MCP tool calls. A separate enforcement
// point from the REST `RateLimitMiddleware` because MCP tool calls all
// land on the same HTTP path (`/mcp/jsonrpc`) — keying on the URL path
// can't distinguish two tools. This limiter keys on (tool_name, principal)
// instead.
//
// Thread-safe (mutex-guarded); shared by all concurrent tool calls in
// the process. Disabled `RateLimitConfig` short-circuits to allow.
class MCPToolRateLimiter {
public:
    struct AcquireDecision {
        bool allowed = false;
        std::int64_t remaining = 0;
        std::int64_t retry_after_seconds = 0;
    };

    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    MCPToolRateLimiter();
    explicit MCPToolRateLimiter(Clock clock);

    // Try to consume one slot in the bucket identified by
    // (tool_name, principal). Returns whether the call is allowed,
    // how many slots remain in the current window, and (when denied)
    // how many seconds until the window resets.
    AcquireDecision tryAcquire(const std::string& tool_name,
                               const std::string& principal,
                               const RateLimitConfig& config);

private:
    struct Bucket {
        std::int64_t remaining = 0;
        std::chrono::steady_clock::time_point reset_time;
    };

    static std::string keyFor(const std::string& tool_name,
                              const std::string& principal);

    Clock clock_;
    std::mutex mutex_;
    std::unordered_map<std::string, Bucket> buckets_;
};

} // namespace flapi
