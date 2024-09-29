#include "auth_middleware.hpp"
#include <crow/http_request.h>
#include <crow/http_response.h>
#include <crow/utility.h>
#include <algorithm>
#include <cstring>
#include <jwt-cpp/jwt.h>

namespace flapi {

void AuthMiddleware::setConfig(std::shared_ptr<ConfigManager> config_manager) {
    this->config_manager = config_manager;
}

void AuthMiddleware::before_handle(crow::request& req, crow::response& res, context& ctx) {
    if (!config_manager) return;

    const auto* endpoint = config_manager->getEndpointForPath(req.url);
    if (!endpoint) {
        CROW_LOG_DEBUG << "No endpoint found for path: " << req.url;
        return;
    }

    if (!endpoint->auth.enabled) {
        CROW_LOG_DEBUG << "Auth not enabled for endpoint: " << req.url;
        return;
    }

    CROW_LOG_DEBUG << "Auth enabled for endpoint: " << req.url;

    auto auth_header = req.get_header_value("Authorization");
    
    if (auth_header.empty()) {
        CROW_LOG_DEBUG << "No Authorization header found";
        res.code = 401;
        res.set_header("WWW-Authenticate", "Basic realm=\"flAPI\"");
        res.end();
        return;
    }

    if (endpoint->auth.type == "basic") {
        ctx.authenticated = authenticateBasic(auth_header, *endpoint, ctx);
    } else if (endpoint->auth.type == "bearer") {
        ctx.authenticated = authenticateBearer(auth_header, *endpoint, ctx);
    } else {
        // Implement other authentication methods here
    }

    if (!ctx.authenticated) {
        CROW_LOG_DEBUG << "Authentication failed";
        res.code = 401;
        res.end();
    } else {
        CROW_LOG_DEBUG << "Authentication successful for user: " << ctx.username;
    }
}

void AuthMiddleware::after_handle(crow::request& req, crow::response& res, context& ctx) {
    // No action needed after handling the request
}

bool AuthMiddleware::authenticateBasic(const std::string& auth_header, const EndpointConfig& endpoint, context& ctx) {
    std::string decoded = crow::utility::base64decode(auth_header.substr(6));
    size_t colon_pos = decoded.find(':');
    
    if (colon_pos == std::string::npos) {
        return false;
    }

    std::string username = decoded.substr(0, colon_pos);
    std::string password = decoded.substr(colon_pos + 1);

    for (const auto& user : endpoint.auth.users) {
        if (user.username == username && user.password == password) {
            ctx.username = username;
            ctx.roles = user.roles;
            return true;
        }
    }

    return false;
}

bool AuthMiddleware::authenticateBearer(const std::string& auth_header, const EndpointConfig& endpoint, context& ctx) {
    if (auth_header.substr(0, 7) != "Bearer ") {
        return false;
    }

    std::string token = auth_header.substr(7);
    
    try {
        auto decoded = jwt::decode(token);
        
        auto verifier = jwt::verify()
            .allow_algorithm(jwt::algorithm::hs256{endpoint.auth.jwt_secret})
            .with_issuer(endpoint.auth.jwt_issuer);

        verifier.verify(decoded);

        // Extract claims
        ctx.username = decoded.get_payload_claim("sub").as_string();
        auto roles = decoded.get_payload_claim("roles").as_array();
        for (auto& role : roles) {
            ctx.roles.push_back(role.get<std::string>());
        }

        return true;
    } catch (const std::exception& e) {
        CROW_LOG_DEBUG << "JWT verification failed: " << e.what();
        return false;
    }
}

void AuthMiddleware::loadHtpasswdFile(const std::string& file_path) {
    std::ifstream file(file_path);
    std::string line;

    while (std::getline(file, line)) {
        size_t colon_pos = line.find(':');
        if (colon_pos != std::string::npos) {
            std::string username = line.substr(0, colon_pos);
            std::string password = line.substr(colon_pos + 1);
            users[username] = password;
        }
    }
}

// Implement other authentication methods here
// bool AuthMiddleware::authenticate_oauth2(const std::string& auth_header, context& ctx) { ... }
// bool AuthMiddleware::authenticate_saml(const std::string& auth_header, context& ctx) { ... }
// bool AuthMiddleware::authenticate_jwt(const std::string& auth_header, context& ctx) { ... }
// bool AuthMiddleware::authenticate_custom(const std::string& auth_header, context& ctx) { ... }

} // namespace flapi