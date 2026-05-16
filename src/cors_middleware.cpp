#include "cors_middleware.hpp"

#include "config_manager.hpp"

namespace flapi {

void FlapiCorsMiddleware::initialize(std::shared_ptr<ConfigManager> config_manager) {
    if (!config_manager) {
        allow_origins_.clear();
        return;
    }
    allow_origins_ = config_manager->getCorsConfig().allow_origins;
}

void FlapiCorsMiddleware::before_handle(crow::request& /*req*/, crow::response& /*res*/, context& /*ctx*/) {
    // No-op. The policy applies on the response.
}

void FlapiCorsMiddleware::after_handle(crow::request& req, crow::response& res, context& /*ctx*/) {
    std::string request_origin;
    auto it = req.headers.find("Origin");
    if (it != req.headers.end()) {
        request_origin = it->second;
    }

    const auto resolved = policy_.resolveAllowedOrigin(request_origin, allow_origins_);
    if (!resolved.has_value()) {
        return;  // No CORS header — browser blocks cross-origin access.
    }

    // Only set if Crow's CORSHandler hasn't already (it shouldn't have, by
    // construction of the middleware order; defensive set_header avoids
    // doubled headers regardless).
    auto existing = res.headers.find("Access-Control-Allow-Origin");
    if (existing == res.headers.end()) {
        res.add_header("Access-Control-Allow-Origin", *resolved);
    }
}

} // namespace flapi
