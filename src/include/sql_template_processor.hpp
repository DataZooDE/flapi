#pragma once

#include <functional>
#include <string>
#include <map>
#include <memory>
#include <string_view>
#include <vector>
#include <crow.h>
#include "mustache_syntax_check.hpp"
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
    // The same rendering for template TEXT that is not (yet) on disk: the
    // editor's unsaved candidate in template/expand?validate_only. Validating
    // the pasted rendering instead reported binder errors (`c = ALFKI`) for SQL
    // that never runs.
    PreparedQueryRender processTemplatePrepared(const std::string& templateContent,
                                                const EndpointConfig& endpoint,
                                                std::map<std::string, std::string>& params);

private:
    std::shared_ptr<ConfigManager> config_manager;

    std::string loadTemplateContent(const std::string& templatePath);
    // The template text, from the in-process cache when the file on disk is
    // unchanged (#243). Never null; throws as loadTemplateContent does.
    std::shared_ptr<const std::string> loadTemplateShared(const std::string& templatePath);
    static std::string readTemplateUncached(IFileProvider& provider, const std::string& templatePath);
    std::string getFullTemplatePath(const std::string& templateSource) const;
    // The auth.* markers of one render and the values that replace them.
    struct AuthRender {
        std::string prefix;
        std::vector<AuthMarker> markers;
    };
    crow::mustache::context createTemplateContext(const EndpointConfig& endpoint,
                                                  std::map<std::string, std::string>& params,
                                                  AuthRender* auth);
    std::string processTemplate(const std::string& templateContent, const crow::mustache::context& ctx,
                                const AuthRender& auth, const std::string& label);
public:
    // auth.* renders EMPTY in this context: an identity reaches SQL only through
    // the render paths above, which check where it lands (variable listings use this).
    crow::mustache::context createTemplateContext(const EndpointConfig& endpoint, std::map<std::string, std::string>& params);
    std::string processTemplate(const std::string& templateContent, const crow::mustache::context& ctx);
    
    // Add these two declarations
    crow::mustache::template_t compileTemplate(const std::string& templateSource);
    /// The environment variables `whitelist` exposes to templates, straight
    /// from `environ` (no copy of the environment). Static: the secret
    /// collector needs it from call sites that hold no template processor.
    static void forEachAllowedEnvironmentVariable(
        const TemplateConfig& whitelist,
        const std::function<void(const std::string& name, std::string_view value)>& visit);

    /// Drop the cached text of every template. Called whenever flAPI writes a
    /// template or reloads endpoints itself: the per-read stat cannot tell two
    /// same-size writes within one filesystem timestamp tick apart.
    static void clearTemplateCache();
    
    // Public method to load template content for validation
    std::string loadTemplate(const EndpointConfig& endpoint);
};

} // namespace flapi