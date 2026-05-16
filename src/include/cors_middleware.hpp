#pragma once

#include <crow/http_request.h>
#include <crow/http_response.h>
#include <memory>
#include <string>
#include <vector>

#include "cors_policy.hpp"

namespace flapi {

class ConfigManager;

// W1.2: Crow middleware that enforces the `cors.allow-origins` allowlist.
// Runs alongside `crow::CORSHandler` — it provides only the per-request
// `Access-Control-Allow-Origin` header, while Crow's CORSHandler keeps
// handling methods, headers, and max-age.
//
// The middleware reads the request's `Origin` header, asks `CorsPolicy`
// for the value it should advertise, and writes it directly onto the
// response. Crow's CORSHandler then sees the header is already set and
// declines to override it (see `set_header_no_override` in Crow).
class FlapiCorsMiddleware {
public:
    struct context {};

    // Must be initialised before the server accepts traffic. Safe to call
    // multiple times if the operator hot-reloads the config — the new
    // allow-origins list takes effect for the next request.
    void initialize(std::shared_ptr<ConfigManager> config_manager);

    void before_handle(crow::request& req, crow::response& res, context& ctx);
    void after_handle(crow::request& req, crow::response& res, context& ctx);

private:
    std::vector<std::string> allow_origins_;
    CorsPolicy policy_;
};

} // namespace flapi
