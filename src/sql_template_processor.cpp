#include "sql_template_processor.hpp"
#include <fstream>
#include <stdexcept>
#include <crow.h>

namespace flapi {

SQLTemplateProcessor::SQLTemplateProcessor(std::shared_ptr<ConfigManager> config_manager)
    : config_manager(config_manager) {}

std::string SQLTemplateProcessor::loadAndProcessTemplate(const EndpointConfig& endpoint, std::map<std::string, std::string>& params) {
    std::string templateContent = loadTemplateContent(endpoint);
    crow::mustache::context ctx = createTemplateContext(endpoint, params);
    return processTemplate(templateContent, ctx);
}

std::string SQLTemplateProcessor::loadAndProcessTemplate(const EndpointConfig& endpoint, const CacheConfig& cacheConfig, std::map<std::string, std::string>& params) {
    std::string templateContent = loadTemplateContent(cacheConfig);
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

std::string SQLTemplateProcessor::loadTemplateContent(const EndpointConfig& endpoint) {
    std::string templatePath = config_manager->getTemplatePath() + "/" + endpoint.templateSource;
    return loadTemplateContent(templatePath);
}

std::string SQLTemplateProcessor::loadTemplateContent(const CacheConfig& cacheConfig) {
    std::string basePath = config_manager->getTemplatePath();
    std::string templatePath = basePath + "/" + cacheConfig.cacheSource;
    return loadTemplateContent(templatePath);
}

std::string SQLTemplateProcessor::loadTemplateContent(const std::string& templatePath) {
    std::ifstream templateFile(templatePath);
    if (!templateFile) {
        throw std::runtime_error("Template file not found: " + templatePath);
    }
    return std::string((std::istreambuf_iterator<char>(templateFile)), std::istreambuf_iterator<char>());
}

crow::mustache::context SQLTemplateProcessor::createTemplateContext(const EndpointConfig& endpoint, std::map<std::string, std::string>& params) {
    crow::mustache::context ctx;

    for (const auto& [key, value] : config_manager->getPropertiesForTemplates(endpoint.connection[0])) {
        ctx["conn"][key] = value;
    }

    for (const auto& [key, value] : params) {
        ctx["params"][key] = value;
    }

    if (params.find("cacheSchema") != params.end()) {
        ctx["cache"]["schema"] = params.find("cacheSchema")->second;
        params.erase(params.find("cacheSchema"));
    }

    if (params.find("cacheTableName") != params.end()) {
        ctx["cache"]["table"] = params.find("cacheTableName")->second;
        params.erase(params.find("cacheTableName"));
    }
        
    if (params.find("cacheRefreshTime") != params.end()) {
        ctx["cache"]["refreshTime"] = params.find("cacheRefreshTime")->second;
        params.erase(params.find("cacheRefreshTime"));
    }

    CROW_LOG_DEBUG << "Template context: " << ctx.dump();

    return ctx;
}


} // namespace flapi