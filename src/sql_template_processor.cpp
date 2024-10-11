#include "sql_template_processor.hpp"
#include <fstream>
#include <stdexcept>
#include <filesystem>

namespace flapi {

SQLTemplateProcessor::SQLTemplateProcessor(std::shared_ptr<ConfigManager> config_manager)
    : config_manager(config_manager) {}

std::string SQLTemplateProcessor::loadAndProcessTemplate(const EndpointConfig& endpoint, std::map<std::string, std::string>& params) {
    std::string templatePath = getFullTemplatePath(endpoint.templateSource);
    CROW_LOG_DEBUG << "Template path: " << templatePath;
    std::string templateContent = loadTemplateContent(templatePath);
    crow::mustache::context ctx = createTemplateContext(endpoint, params);
    return processTemplate(templateContent, ctx);
}

std::string SQLTemplateProcessor::loadAndProcessTemplate(const EndpointConfig& endpoint, const CacheConfig& cacheConfig, std::map<std::string, std::string>& params) {
    std::string templatePath = getFullTemplatePath(cacheConfig.cacheSource);
    std::string templateContent = loadTemplateContent(templatePath);
    crow::mustache::context ctx = createTemplateContext(endpoint, params);
    return processTemplate(templateContent, ctx);
}

std::string SQLTemplateProcessor::processTemplate(const std::string& templateContent, const crow::mustache::context& ctx) {
    auto compiledTemplate = compileTemplate(templateContent);
    auto renderedTemplate = compiledTemplate.render(ctx).dump();
    CROW_LOG_DEBUG << "Processed query: \n" << renderedTemplate;
    return renderedTemplate;
}

crow::mustache::template_t SQLTemplateProcessor::compileTemplate(const std::string& templateSource) {
    return crow::mustache::compile(templateSource);
}

std::string SQLTemplateProcessor::loadTemplateContent(const std::string& templatePath) {
    std::ifstream templateFile(templatePath);
    if (!templateFile) {
        throw std::runtime_error("Template file not found: " + templatePath);
    }
    return std::string((std::istreambuf_iterator<char>(templateFile)), std::istreambuf_iterator<char>());
}

std::string SQLTemplateProcessor::getFullTemplatePath(const std::string& templateSource) const {
    std::filesystem::path basePath = config_manager->getTemplatePath();
    std::filesystem::path fullPath = basePath / templateSource;
    return fullPath.string();
}

crow::mustache::context SQLTemplateProcessor::createTemplateContext(const EndpointConfig& endpoint, std::map<std::string, std::string>& params) {
    crow::mustache::context ctx;

    // Add connection properties
    for (const auto& [key, value] : config_manager->getPropertiesForTemplates(endpoint.connection[0])) {
        ctx["conn"][key] = value;
    }

    // Add filtered environment variables
    const auto& templateConfig = config_manager->getTemplateConfig();
    for (const auto& [key, value] : getEnvironmentVariables()) {
        if (templateConfig.isEnvironmentVariableAllowed(key)) {
            ctx["env"][key] = value;
        }
    }

    // Add cache-related parameters
    if (params.find("cacheSchema") != params.end()) {
        ctx["cache"]["schema"] = params["cacheSchema"];
        params.erase("cacheSchema");
    }

    if (params.find("cacheTableName") != params.end()) {
        ctx["cache"]["table"] = params["cacheTableName"];
        params.erase("cacheTableName");
    }
        
    if (params.find("cacheRefreshTime") != params.end()) {
        ctx["cache"]["refreshTime"] = params["cacheRefreshTime"];
        params.erase("cacheRefreshTime");
    }

    if (params.find("currentWatermark") != params.end()) {
        ctx["cache"]["currentWatermark"] = params["currentWatermark"];
        params.erase("currentWatermark");
    }

    if (params.find("previousCacheTableName") != params.end()) {
        ctx["cache"]["previousTable"] = params["previousCacheTableName"];
        params.erase("previousCacheTableName");
    }

    if (params.find("previousWatermark") != params.end()) {
        ctx["cache"]["previousWatermark"] = params["previousWatermark"];
        params.erase("previousWatermark");
    }

    // Add request parameters
    for (const auto& [key, value] : params) {
        ctx["params"][key] = value;
    }

    CROW_LOG_DEBUG << "Template context: " << ctx.dump();

    return ctx;
}

std::map<std::string, std::string> SQLTemplateProcessor::getEnvironmentVariables() {
    std::map<std::string, std::string> envMap;
    for (char** env = environ; *env != nullptr; ++env) {
        std::string envStr(*env);
        size_t pos = envStr.find('=');
        if (pos != std::string::npos) {
            std::string key = envStr.substr(0, pos);
            std::string value = envStr.substr(pos + 1);
            envMap[key] = value;
        }
    }
    return envMap;
}

} // namespace flapi