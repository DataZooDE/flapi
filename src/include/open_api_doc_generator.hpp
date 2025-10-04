#pragma once

#include <crow.h>
#include "crow/middlewares/cors.h"
#include <algorithm>
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
    YAML::Node generateConfigServiceDoc(crow::App<crow::CORSHandler, RateLimitMiddleware, AuthMiddleware>& app);

private:
    std::shared_ptr<ConfigManager> configManager;
    std::shared_ptr<DatabaseManager> dbManager;
    
    YAML::Node generatePathItem(const EndpointConfig& endpoint);
    YAML::Node generateParameters(const std::vector<RequestFieldConfig>& requestFields);
    YAML::Node generateResponseSchema(const EndpointConfig& endpoint);
    
    // Config service specific documentation
    void addConfigServicePaths(YAML::Node& paths);
    YAML::Node createOperation(const std::string& summary, const std::string& description,
                               const std::string& tag = "");
    void addParameter(YAML::Node& parameters, const std::string& name, const std::string& in,
                     const std::string& type, bool required, const std::string& description);
    void addResponse(YAML::Node& responses, const std::string& code, const std::string& description,
                    const std::string& schema_ref = "");
    void addRequestBody(YAML::Node& operation, const std::string& description,
                       const std::string& schema_ref = "");
};

} // namespace flapi