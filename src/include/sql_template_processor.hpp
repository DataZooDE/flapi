#pragma once

#include <string>
#include <map>
#include <vector>
#include <crow.h>
#include "config_manager.hpp"

#ifdef _WIN32
#include <stdlib.h>
#define environ _environ
#else
extern char **environ;
#endif

namespace flapi {

class SQLTemplateProcessor {
public:
    SQLTemplateProcessor(std::shared_ptr<ConfigManager> config_manager);
    
    std::string loadAndProcessTemplate(const EndpointConfig& endpoint, std::map<std::string, std::string>& params);
    std::string loadAndProcessTemplate(const EndpointConfig& endpoint, const CacheConfig& cacheConfig, std::map<std::string, std::string>& params);
private:
    std::shared_ptr<ConfigManager> config_manager;

    std::string loadTemplateContent(const EndpointConfig& endpoint);
    std::string loadTemplateContent(const CacheConfig& cacheConfig);
    std::string loadTemplateContent(const std::string& templatePath);

    crow::mustache::context createTemplateContext(const EndpointConfig& endpoint, std::map<std::string, std::string>& params);
    std::map<std::string, std::string> getEnvironmentVariables();

    std::string processTemplate(const std::string& templateContent, const crow::mustache::context& ctx);
    crow::mustache::template_t compileTemplate(const std::string& templateSource);
};

} // namespace flapi