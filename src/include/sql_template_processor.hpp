#pragma once

#include <string>
#include <map>
#include <memory>
#include <vector>
#include <crow.h>
#include "config_manager.hpp"
#include "prepared_template_rewriter.hpp"

#ifdef _WIN32
#include <stdlib.h>
#define environ _environ
#else
extern char **environ;
#endif

namespace flapi {

// W3.1 PR B: prepared rendering output. `sql` carries `?` placeholders
// where typed `{{ params.X }}` references stood in the source template;
// `bindings` records the per-? field name and SqlParameterType so the
// caller can bind values via DuckDB's prepared-statement API. When
// `bindings` is empty, the prepared path collapses to the historic
// "execute string" path — no behaviour change for endpoints without
// typed validators.
struct PreparedQueryRender {
    std::string sql;
    std::vector<PreparedBindingSpec> bindings;
};

class SQLTemplateProcessor {
public:
    SQLTemplateProcessor(std::shared_ptr<ConfigManager> config_manager);

    std::string loadAndProcessTemplate(const EndpointConfig& endpoint, std::map<std::string, std::string>& params);
    std::string loadAndProcessTemplate(const EndpointConfig& endpoint, const CacheConfig& cacheConfig, std::map<std::string, std::string>& params);

    // W3.1 PR B prepared-path entry point. Loads the template, rewrites
    // typed scalar `{{ params.X }}` occurrences to `?`, runs Mustache on
    // the rewritten template, and returns the SQL together with the
    // binding plan. Cache + auth + sections are still resolved at this
    // step exactly as the legacy path — only top-level typed-scalar
    // params take the placeholder route.
    PreparedQueryRender loadAndProcessTemplatePrepared(
        const EndpointConfig& endpoint, std::map<std::string, std::string>& params);

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
    
    // Public method to load template content for validation
    std::string loadTemplate(const EndpointConfig& endpoint);
};

} // namespace flapi