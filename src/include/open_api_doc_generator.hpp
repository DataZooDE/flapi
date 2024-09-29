#pragma once

#include <crow.h>
#include "crow/middlewares/cors.h"

#include <yaml-cpp/yaml.h>
#include <memory>

#include "config_manager.hpp"
#include "database_manager.hpp"
#include "auth_middleware.hpp"
#include "rate_limit_middleware.hpp"

namespace flapi {

class OpenAPIDocGenerator {
public:
    OpenAPIDocGenerator(std::shared_ptr<ConfigManager> cm, std::shared_ptr<DatabaseManager> dm);
    YAML::Node generateDoc(crow::App<crow::CORSHandler, RateLimitMiddleware, AuthMiddleware>& app);

private:
    std::shared_ptr<ConfigManager> configManager;
    std::shared_ptr<DatabaseManager> dbManager;
    
    YAML::Node generatePathItem(const EndpointConfig& endpoint);
    YAML::Node generateParameters(const std::vector<RequestFieldConfig>& requestFields);
    YAML::Node generateResponseSchema(const EndpointConfig& endpoint);
};

} // namespace flapi