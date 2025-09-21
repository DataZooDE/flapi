#pragma once

#include <string>
#include <map>
#include <memory>
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

    std::string loadTemplateContent(const std::string& templatePath);
    std::string getFullTemplatePath(const std::string& templateSource) const;
public:
    crow::mustache::context createTemplateContext(const EndpointConfig& endpoint, std::map<std::string, std::string>& params);
    std::string processTemplate(const std::string& templateContent, const crow::mustache::context& ctx);
    
    // Add these two declarations
    crow::mustache::template_t compileTemplate(const std::string& templateSource);
    std::map<std::string, std::string> getEnvironmentVariables();
};

} // namespace flapi