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
// Runs alongside `crow::CORSHandler` and has the last word (see
// flapi_app.hpp): it sets the per-request `Access-Control-Allow-Origin` and
// `Vary: Origin`, and on a preflight (OPTIONS with
// `Access-Control-Request-Method`) also Allow-Methods, Allow-Headers and
// Max-Age. Crow's handler only knows static values, and its `*` for headers
// does not cover `Authorization`.
//
// Preflights only reach this middleware WITH their request headers because
// FlapiApp defers Crow's built-in OPTIONS answer (cors_middleware.cpp): Crow
// otherwise answers OPTIONS as soon as the request line is parsed, before
// any header - `Origin` included - has been read.
class FlapiCorsMiddleware {
public:
    struct context {};

    // Must be initialised before the server accepts traffic. Safe to call
    // multiple times if the operator hot-reloads the config — the new
    // allow-origins list takes effect for the next request.
    void initialize(std::shared_ptr<ConfigManager> config_manager);

    void before_handle(crow::request& req, crow::response& res, context& ctx);
    void after_handle(crow::request& req, crow::response& res, context& ctx);

    // OPTIONS carrying `Access-Control-Request-Method`.
    static bool isPreflight(const crow::request& req);

private:
    std::vector<std::string> allow_origins_;
    std::vector<std::string> allow_headers_;
    std::vector<std::string> allow_methods_;
    bool auth_enabled_ = false;
    CorsPolicy policy_;
};

} // namespace flapi
