#include "cors_middleware.hpp"

#include "config_manager.hpp"
#include "flapi_app.hpp"

namespace flapi {

void FlapiCorsMiddleware::initialize(std::shared_ptr<ConfigManager> config_manager) {
    if (!config_manager) {
        allow_origins_.clear();
        allow_headers_.clear();
        allow_methods_.clear();
        auth_enabled_ = false;
        return;
    }
    const auto& cors = config_manager->getCorsConfig();
    allow_origins_ = cors.allow_origins;
    allow_headers_ = cors.allow_headers;
    allow_methods_ = cors.allow_methods;

    bool any_auth = config_manager->isAuthEnabled() || config_manager->getMCPConfig().auth.enabled;
    for (const auto& endpoint : *config_manager->getEndpoints()) {
        any_auth = any_auth || endpoint.auth.enabled;
    }
    auth_enabled_ = any_auth;
}

bool FlapiCorsMiddleware::isPreflight(const crow::request& req) {
    return req.method == crow::HTTPMethod::Options &&
           req.headers.find("Access-Control-Request-Method") != req.headers.end();
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
    if (*resolved != CorsPolicy::kWildcard) {
        // The answer depends on the request's Origin: a cache that stored it
        // for one origin must not serve it to another.
        res.add_header("Vary", "Origin");
    }

    if (isPreflight(req)) {
        res.set_header("Access-Control-Allow-Methods", policy_.resolveAllowMethods(allow_methods_));
        res.set_header("Access-Control-Allow-Headers",
                       policy_.resolveAllowHeaders(allow_headers_,
                                                   req.get_header_value("Access-Control-Request-Headers"),
                                                   auth_enabled_));
        res.set_header("Access-Control-Max-Age", std::to_string(CorsPolicy::kPreflightMaxAgeSeconds));
    }
}

} // namespace flapi

// Crow answers OPTIONS inside handle_initial, which its connection calls as soon
// as the request LINE is parsed - before any header has been read (crow/parser.h
// on_url -> http_connection.h handle_url). The middlewares then ran on a request
// with no headers at all: a preflight's `Origin` and `Access-Control-Request-*`
// were invisible, and every allowlisted preflight was answered 204 without a
// single Access-Control-Allow-* header, so browsers blocked every authenticated
// or JSON cross-origin call.
//
// For OPTIONS, handle_initial now only DEFERS: it returns a placeholder route so
// the connection reads the headers and runs the middlewares as for any other
// request, and handle() then produces Crow's own answer (204 + Allow, or 404).
// Every other method goes to Crow's router unchanged.
namespace {
constexpr uint16_t kDeferredOptionsRule = 0xFFFF;
}  // namespace

template<>
std::unique_ptr<crow::routing_handle_result> flapi::FlapiApp::handle_initial(crow::request& req,
                                                                              crow::response& res) {
    if (req.method != crow::HTTPMethod::Options) {
        return router_.handle_initial(req, res);
    }
    return std::make_unique<crow::routing_handle_result>(
        kDeferredOptionsRule, std::vector<uint16_t>(), crow::routing_params(), crow::HTTPMethod::Options);
}

template<>
void flapi::FlapiApp::handle(crow::request& req, crow::response& res,
                             std::unique_ptr<crow::routing_handle_result>& found) {
    if (found->method != crow::HTTPMethod::Options || found->rule_index != kDeferredOptionsRule) {
        router_.handle<self_t>(req, res, *found);
        return;
    }
    // Crow's built-in OPTIONS answer, computed now that the request is complete.
    crow::response answer;
    router_.handle_initial(req, answer);
    res.code = answer.code;
    const auto allow = crow::get_header_value(answer.headers, "Allow");
    if (!allow.empty()) {
        res.set_header("Allow", allow);
    }
    res.end();
}
