#pragma once

#include <crow/http_request.h>
#include <crow/http_response.h>
#include "config_manager.hpp"
#include <string>
#include <unordered_map>

namespace flapi {



class AuthMiddleware {
public:
    struct context {
        bool authenticated = false;
        std::string username;
        std::vector<std::string> roles;
    };

    void setConfig(std::shared_ptr<ConfigManager> config_manager);
    void before_handle(crow::request& req, crow::response& res, context& ctx);
    void after_handle(crow::request& req, crow::response& res, context& ctx);

private:
    std::shared_ptr<ConfigManager> config_manager;
    std::unordered_map<std::string, std::string> users;

    bool authenticateBasic(const std::string& auth_header, const EndpointConfig& endpoint, context& ctx);
    bool authenticateBearer(const std::string& auth_header, const EndpointConfig& endpoint, context& ctx);
    void loadHtpasswdFile(const std::string& file_path);
};

} // namespace flapi