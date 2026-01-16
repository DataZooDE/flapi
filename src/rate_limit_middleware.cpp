#include "rate_limit_middleware.hpp"

#include <algorithm>

namespace flapi {

void RateLimitMiddleware::setConfig(std::shared_ptr<ConfigManager> config_manager) {
    this->config_manager = config_manager;
}

void RateLimitMiddleware::before_handle(crow::request& req, crow::response& res, context& ctx) {
    if (!config_manager) return;

    const auto& endpoint = config_manager->getEndpointForPath(req.url);
    if (!endpoint || !endpoint->rate_limit.enabled) {
        return;
    }

    // FIX #4: Include path in rate limit key (per-endpoint rate limits)
    std::string client_ip = req.remote_ip_address;
    std::string rate_key = client_ip + "|" + req.url;

    {
        std::lock_guard<std::mutex> lock(mutex);
        updateRateLimit(rate_key, endpoint->rate_limit.max,
                        endpoint->rate_limit.interval, ctx);
    }

    // Convert steady_clock to system_clock for proper Unix timestamp
    auto now_steady = std::chrono::steady_clock::now();
    auto now_system = std::chrono::system_clock::now();
    auto duration_until_reset = ctx.reset_time - now_steady;
    auto reset_system = now_system + duration_until_reset;
    int64_t reset_seconds = std::chrono::duration_cast<std::chrono::seconds>(
        reset_system.time_since_epoch()).count();

    // Always add rate limit headers (even on 429 - per RFC 6585)
    res.add_header("X-RateLimit-Limit", std::to_string(endpoint->rate_limit.max));
    res.add_header("X-RateLimit-Remaining", std::to_string(std::max(0, ctx.remaining)));
    res.add_header("X-RateLimit-Reset", std::to_string(reset_seconds));

    // FIX #1: Check if exceeded (remaining went negative after decrement)
    if (ctx.remaining < 0) {
        int64_t retry_after = std::chrono::duration_cast<std::chrono::seconds>(
            duration_until_reset).count();

        // Set 429 response - don't call end() as Crow handles that
        res.code = 429;
        res.set_header("Content-Type", "text/plain");
        res.set_header("Retry-After", std::to_string(std::max(int64_t(1), retry_after)));
        res.set_header("Connection", "close");
        res.body = "Rate limit exceeded. Try again later.";
        // Don't call res.end() - let Crow's middleware chain handle completion
        return;
    }
}

void RateLimitMiddleware::after_handle(crow::request& req, crow::response& res, context& ctx) {
    // No action needed after handling the request
}

void RateLimitMiddleware::updateRateLimit(const std::string& rate_key, int max, int interval, context& ctx) {
    auto now = std::chrono::steady_clock::now();
    auto& client_ctx = client_contexts[rate_key];

    // Reset window if expired
    if (now >= client_ctx.reset_time) {
        client_ctx.remaining = max;
        client_ctx.reset_time = now + std::chrono::seconds(interval);
    }

    // Decrement (can go negative to indicate exceeded)
    client_ctx.remaining--;
    ctx = client_ctx;

    // FIX #6: Periodic cleanup of expired entries
    cleanupExpiredEntries(now);
}

void RateLimitMiddleware::cleanupExpiredEntries(std::chrono::steady_clock::time_point now) {
    // Only cleanup every ~100 requests to avoid overhead
    static int counter = 0;
    if (++counter < 100) return;
    counter = 0;

    for (auto it = client_contexts.begin(); it != client_contexts.end(); ) {
        if (now >= it->second.reset_time) {
            it = client_contexts.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace flapi
