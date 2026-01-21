#include "sql_template_processor.hpp"
#include "vfs_adapter.hpp"
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
    std::string templatePath;
    if (cacheConfig.template_file) {
        templatePath = getFullTemplatePath(cacheConfig.template_file.value());
        CROW_LOG_DEBUG << "Using cache template file: " << templatePath;
    } else {
        templatePath = getFullTemplatePath(endpoint.templateSource);
        CROW_LOG_DEBUG << "Using endpoint template file: " << templatePath;
    }
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
    auto file_provider = config_manager->getFileProvider();
    if (!file_provider->FileExists(templatePath)) {
        throw std::runtime_error("Template file not found: " + templatePath);
    }
    return file_provider->ReadFile(templatePath);
}

std::string SQLTemplateProcessor::getFullTemplatePath(const std::string& templateSource) const {
    // If templateSource is a remote path (s3://, gs://, https://, etc.), return it directly
    if (PathSchemeUtils::IsRemotePath(templateSource)) {
        return templateSource;
    }

    // If templateSource is an absolute local path, return it directly
    if (std::filesystem::path(templateSource).is_absolute()) {
        return templateSource;
    }

    // Get base template path - could be local or remote
    std::string basePath = config_manager->getTemplatePath();

    // If base path is remote, concatenate strings (can't use std::filesystem)
    if (PathSchemeUtils::IsRemotePath(basePath)) {
        if (!basePath.empty() && basePath.back() != '/') {
            basePath += '/';
        }
        return basePath + templateSource;
    }

    // Local path resolution
    std::filesystem::path fullPath = std::filesystem::path(basePath) / templateSource;
    return fullPath.string();
}

crow::mustache::context SQLTemplateProcessor::createTemplateContext(const EndpointConfig& endpoint, std::map<std::string, std::string>& params) {
    crow::mustache::context ctx;

    // Add connection properties
    if (!endpoint.connection.empty()) {
        for (const auto& [key, value] : config_manager->getPropertiesForTemplates(endpoint.connection[0])) {
            ctx["conn"][key] = value;
        }
    }

    // Add filtered environment variables
    const auto& templateConfig = config_manager->getTemplateConfig();
    for (const auto& [key, value] : getEnvironmentVariables()) {
        if (templateConfig.isEnvironmentVariableAllowed(key)) {
            ctx["env"][key] = value;
        }
    }

    // Add cache-related parameters
    if (params.find("cacheCatalog") != params.end()) {
        ctx["cache"]["catalog"] = params["cacheCatalog"];
        params.erase("cacheCatalog");
    }

    if (params.find("cacheSchema") != params.end()) {
        ctx["cache"]["schema"] = params["cacheSchema"];
        params.erase("cacheSchema");
    }

    if (params.find("cacheTable") != params.end()) {
        ctx["cache"]["table"] = params["cacheTable"];
        params.erase("cacheTable");
    }

    if (params.find("cacheSchedule") != params.end()) {
        ctx["cache"]["schedule"] = params["cacheSchedule"];
        params.erase("cacheSchedule");
    }

    if (params.find("cacheSnapshotId") != params.end()) {
        ctx["cache"]["snapshotId"] = params["cacheSnapshotId"];
        params.erase("cacheSnapshotId");
    }

    if (params.find("cacheSnapshotTimestamp") != params.end()) {
        ctx["cache"]["snapshotTimestamp"] = params["cacheSnapshotTimestamp"];
        params.erase("cacheSnapshotTimestamp");
    }

    if (params.find("previousSnapshotId") != params.end()) {
        ctx["cache"]["previousSnapshotId"] = params["previousSnapshotId"];
        params.erase("previousSnapshotId");
    }

    if (params.find("previousSnapshotTimestamp") != params.end()) {
        ctx["cache"]["previousSnapshotTimestamp"] = params["previousSnapshotTimestamp"];
        params.erase("previousSnapshotTimestamp");
    }

    if (params.find("cursorColumn") != params.end()) {
        ctx["cache"]["cursorColumn"] = params["cursorColumn"];
        params.erase("cursorColumn");
    }

    if (params.find("cursorType") != params.end()) {
        ctx["cache"]["cursorType"] = params["cursorType"];
        params.erase("cursorType");
    }

    if (params.find("primaryKeys") != params.end()) {
        ctx["cache"]["primaryKeys"] = params["primaryKeys"];
        params.erase("primaryKeys");
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

std::string SQLTemplateProcessor::loadTemplate(const EndpointConfig& endpoint) {
    std::string templatePath = getFullTemplatePath(endpoint.templateSource);
    return loadTemplateContent(templatePath);
}

} // namespace flapi