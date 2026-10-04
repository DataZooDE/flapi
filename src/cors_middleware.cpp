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
        // No CORS header - the browser blocks cross-origin access. Crow's own CORS
        // handler may already have set its wildcard defaults on this response, which
        // would hand a disallowed origin `Access-Control-Allow-Origin: *`; take them off.
        res.headers.erase("Access-Control-Allow-Origin");
        res.headers.erase("Access-Control-Allow-Headers");
        res.headers.erase("Access-Control-Allow-Methods");
        res.headers.erase("Access-Control-Allow-Credentials");
        res.headers.erase("Access-Control-Max-Age");
        return;
    }

    // Overwrite any value Crow's CORSHandler may already have set. The
    // built-in handler keeps a static origin string (defaults to "*") and
    // applies it without inspecting the request's Origin header — when an
    // operator configures `cors.allow-origins`, the per-request value
    // resolved here must take precedence so a non-allowlisted Origin does
    // not see "*" echoed back.
    res.set_header("Access-Control-Allow-Origin", *resolved);
}

} // namespace flapi
