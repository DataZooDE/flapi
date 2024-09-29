#pragma once

#include <crow.h>
#include "config_manager.hpp"

namespace flapi {

class RateLimitMiddleware {
public:
    struct context {
        int remaining;
        std::chrono::steady_clock::time_point reset_time;
    };

    void setConfig(std::shared_ptr<ConfigManager> config_manager);

    void before_handle(crow::request& req, crow::response& res, context& ctx);
    void after_handle(crow::request& req, crow::response& res, context& ctx);

private:
    std::shared_ptr<ConfigManager> config_manager;
    std::unordered_map<std::string, context> client_contexts;
    std::mutex mutex;

    void updateRateLimit(const std::string& client_ip, const std::string& path, context& ctx);
};

} // namespace flapi