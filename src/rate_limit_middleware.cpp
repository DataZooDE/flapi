#include "rate_limit_middleware.hpp"

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

    std::string client_ip = req.remote_ip_address;
    
    std::lock_guard<std::mutex> lock(mutex);
    updateRateLimit(client_ip, req.url, ctx);

    if (ctx.remaining <= 0) {
        res.code = 429;
        res.write("Rate limit exceeded. Try again later.");
        res.end();
    } else {
        res.add_header("X-RateLimit-Limit", std::to_string(endpoint->rate_limit.max));
        res.add_header("X-RateLimit-Remaining", std::to_string(ctx.remaining));
        res.add_header("X-RateLimit-Reset", std::to_string(std::chrono::duration_cast<std::chrono::seconds>(ctx.reset_time.time_since_epoch()).count()));
    }
}

void RateLimitMiddleware::after_handle(crow::request& req, crow::response& res, context& ctx) {
    // No action needed after handling the request
}

void RateLimitMiddleware::updateRateLimit(const std::string& client_ip, const std::string& path, context& ctx) {
    auto now = std::chrono::steady_clock::now();
    auto& client_ctx = client_contexts[client_ip];

    const auto& endpoint = config_manager->getEndpointForPath(path);
    if (!endpoint) return;

    int max = endpoint->rate_limit.max;
    int interval = endpoint->rate_limit.interval;

    if (now >= client_ctx.reset_time) {
        client_ctx.remaining = max;
        client_ctx.reset_time = now + std::chrono::seconds(interval);
    }

    ctx = client_ctx;
    if (ctx.remaining > 0) {
        ctx.remaining--;
    }

    client_contexts[client_ip] = ctx;
}

} // namespace flapi