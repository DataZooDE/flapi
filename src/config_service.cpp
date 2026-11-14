#include <atomic>
#include <filesystem>
#include <optional>
#include <fstream>
#include <sstream>
#include <regex>
#include <algorithm>
#include <chrono>
#include <set>

#include <yaml-cpp/yaml.h>

#include <openssl/crypto.h>

#include "config_service.hpp"
#include "mustache_syntax_check.hpp"

#ifdef _WIN32
#define ENV_BLOCK _environ
#else
extern char** environ;
#define ENV_BLOCK environ
#endif
#include "flapi_tracing.hpp"
#include "request_context.hpp"
#include "endpoint_config_parser.hpp"
#include "vfs_adapter.hpp"
#include "auth_params.hpp"
#include "json_utils.hpp"
#include "request_validator.hpp"
#include "template_secrets.hpp"
#include "redaction.hpp"
#include "path_utils.hpp"
#include "database_manager.hpp"
#include "database_health.hpp"
#include "cache_manager.hpp"
#include "cache_refresh_command.hpp"
#include "sql_template_processor.hpp"
#include "mcp_dry_run.hpp"
#include "arrow_metrics.hpp"
#include "runtime_metrics.hpp"
#include "vfs_health_checker.hpp"
#include "credential_manager.hpp"

namespace flapi {

namespace {

/// Per-cache refresh roll-up for the metrics response: readiness, the last
/// refresh's mode/status/duration/rows/snapshot, and how old the served data
/// is. Read from CacheManager's in-memory stats, never from DuckLake, so the
/// metrics endpoint stays cheap and works while the catalog is busy.
crow::json::wvalue buildCacheMetricsJson(const std::shared_ptr<ConfigManager>& config_manager,
                                         const std::string& endpoint_filter) {
    std::vector<crow::json::wvalue> rows;
    auto db_manager = DatabaseManager::getInstance();
    auto cache_manager = db_manager ? db_manager->getCacheManager() : nullptr;
    if (!cache_manager) {
        return crow::json::wvalue(rows);
    }
    // Which endpoints cache into each table, from the CONFIG. The endpoint used to
    // come only from refresh stats, so before the first refresh in a process
    // `--endpoint /x` matched nothing and the unfiltered rows had no `endpoint`.
    std::map<std::string, std::vector<std::string>> config_endpoints;
    if (config_manager) {
        const std::string alias = config_manager->getDuckLakeConfig().alias;
        for (const auto& endpoint : *config_manager->getEndpoints()) {
            if (!endpoint.cache.enabled || endpoint.cache.table.empty()) {
                continue;
            }
            const std::string schema = endpoint.cache.schema.empty() ? "main" : endpoint.cache.schema;
            config_endpoints[alias + "." + schema + "." + endpoint.cache.table].push_back(
                endpoint.urlPath.empty() ? endpoint.getName() : endpoint.urlPath);
        }
    }
    const auto states = cache_manager->getReadinessStates();
    std::map<std::string, CacheManager::CacheRefreshStats> by_table;
    for (const auto& stats : cache_manager->getRefreshStats()) {
        by_table[stats.catalog + "." + stats.schema + "." + stats.table] = stats;
    }
    const auto now = std::chrono::system_clock::now();
    // Every known cache appears, refreshed or not: an unbuilt cache with no
    // refresh yet is exactly what an operator is looking for.
    std::set<std::string> tables;
    for (const auto& [name, state] : states) { tables.insert(name); }
    for (const auto& [name, stats] : by_table) { tables.insert(name); }
    for (const auto& name : tables) {
        const auto stats_it = by_table.find(name);
        const auto config_it = config_endpoints.find(name);
        if (!endpoint_filter.empty()) {
            const bool by_stats = stats_it != by_table.end() && stats_it->second.endpoint_path == endpoint_filter;
            const bool by_config =
                config_it != config_endpoints.end() &&
                std::find(config_it->second.begin(), config_it->second.end(), endpoint_filter) !=
                    config_it->second.end();
            if (!by_stats && !by_config) {
                continue;
            }
        }
        crow::json::wvalue row;
        row["cache"] = name;
        if (config_it != config_endpoints.end() && !config_it->second.empty()) {
            row["endpoint"] = config_it->second.front();
        }
        const auto state_it = states.find(name);
        row["state"] = state_it != states.end() ? state_it->second : std::string("unknown");
        if (stats_it != by_table.end()) {
            const auto& st = stats_it->second;
            if (!st.endpoint_path.empty()) {
                row["endpoint"] = st.endpoint_path;
            }
            row["refreshes"] = st.refreshes;
            row["failures"] = st.failures;
            row["last"]["mode"] = st.last_sync_type;
            row["last"]["status"] = st.last_status;
            row["last"]["full"] = st.last_full;
            if (st.last_duration_ms) { row["last"]["duration_ms"] = *st.last_duration_ms; }
            if (st.last_rows_affected) { row["last"]["rows_affected"] = *st.last_rows_affected; }
            if (st.last_snapshot_id) { row["last"]["snapshot_id"] = *st.last_snapshot_id; }
            if (st.last_completed_at) {
                row["last"]["completed_at"] = CacheManager::formatUtcTimestamp(*st.last_completed_at);
            }
            const int64_t timed = st.refreshes;  // refreshes with a duration (all of them, since timing landed)
            row["duration_ms"]["avg"] = timed > 0 ? st.total_duration_ms / timed : 0;
            row["duration_ms"]["max"] = st.max_duration_ms;
        }
        // The last successful refresh, else the newest snapshot in the catalog:
        // after a restart whose refresh failed, an old snapshot IS served.
        {
            const auto first = name.find('.');
            const auto second = name.find('.', first + 1);
            if (first != std::string::npos && second != std::string::npos) {
                if (const auto at = cache_manager->dataTimestamp(name.substr(0, first),
                                                                 name.substr(first + 1, second - first - 1),
                                                                 name.substr(second + 1))) {
                    row["data_age_seconds"] = static_cast<int64_t>(std::max<int64_t>(
                        0, std::chrono::duration_cast<std::chrono::seconds>(now - *at).count()));
                }
            }
        }
        rows.push_back(std::move(row));
    }
    return crow::json::wvalue(rows);
}

/// The cache catalog/schema/table a cached endpoint's serving template reads,
/// exactly as the request path supplies them.
void addCacheTargetParams(const std::shared_ptr<ConfigManager>& config_manager, const EndpointConfig& endpoint,
                          std::map<std::string, std::string>& params) {
    auto db_manager = DatabaseManager::getInstance();
    if (auto cache_manager = db_manager ? db_manager->getCacheManager() : nullptr) {
        cache_manager->addQueryCacheParamsIfNecessary(config_manager, endpoint, params);
    }
}

/// The DuckLake table a cached endpoint's cache writes, or nullopt.
std::optional<PopulateTarget> cacheTableOf(const ConfigManager& config_manager, const EndpointConfig& endpoint) {
    if (!endpoint.cache.enabled || endpoint.cache.table.empty() || !config_manager.getDuckLakeConfig().enabled) {
        return std::nullopt;
    }
    return PopulateTarget{config_manager.getDuckLakeConfig().alias,
                          endpoint.cache.schema.empty() ? "main" : endpoint.cache.schema, endpoint.cache.table};
}

/// validateScript's findings, as template/expand?validate_only lists them.
/// Parse and bind errors keep the type `sql_syntax` existing clients test for;
/// `kind` says which, `statement` is 1-based.
void appendScriptIssues(const DatabaseManager::ScriptValidation& validation, const TemplateSecrets& secrets,
                        crow::json::wvalue& errors, crow::json::wvalue& warnings) {
    for (const auto& issue : validation.errors) {
        crow::json::wvalue error;
        const bool sql = issue.type == "parse" || issue.type == "bind";
        error["type"] = sql ? std::string("sql_syntax") : issue.type;
        error["kind"] = issue.type;
        error["statement"] = static_cast<uint64_t>(issue.statement);
        error["message"] = secrets.scrub(sql ? "SQL validation failed: statement " + std::to_string(issue.statement) +
                                                   ": " + issue.message
                                             : issue.message);
        errors[errors.size()] = std::move(error);
    }
    for (const auto& issue : validation.warnings) {
        crow::json::wvalue warning;
        warning["type"] = issue.type;
        warning["statement"] = static_cast<uint64_t>(issue.statement);
        warning["message"] = secrets.scrub(issue.message);
        warnings[warnings.size()] = std::move(warning);
    }
}

} // namespace

crow::json::wvalue buildMetricsJson(const std::shared_ptr<ConfigManager>& config_manager, const std::string& filter) {
    crow::json::wvalue metrics;

    // Tracing. Exported vs dropped is the signal that matters: a
    // collector that is down must show up here rather than as a trace
    // that quietly has holes in it.
    metrics["tracing"]["enabled"] = Tracing().isEnabled();
    metrics["tracing"]["spans_exported"] =
        static_cast<std::int64_t>(Tracing().spansExported());
    metrics["tracing"]["spans_submitted"] =
        static_cast<std::int64_t>(Tracing().spansSubmitted());
    metrics["tracing"]["spans_dropped"] =
        static_cast<std::int64_t>(Tracing().spansDropped());
    // Only ever non-zero with tracing.flush.blocking_timeout_ms set. A
    // rising value means the collector is costing callers latency AND
    // still losing spans - the worst of both, and a signal to raise the
    // budget or stop blocking.
    metrics["tracing"]["spans_flush_timeouts"] =
        static_cast<std::int64_t>(Tracing().flushTimeouts());

    // Arrow IPC serialization counters, which already existed but were
    // only reachable through /mcp/health.
    const auto& arrow = ArrowMetrics::instance();
    metrics["arrow"]["total_requests"] =
        static_cast<std::int64_t>(arrow.counters.totalRequests.load());
    metrics["arrow"]["successful_requests"] =
        static_cast<std::int64_t>(arrow.counters.successfulRequests.load());
    metrics["arrow"]["failed_requests"] =
        static_cast<std::int64_t>(arrow.counters.failedRequests.load());
    metrics["arrow"]["total_rows"] =
        static_cast<std::int64_t>(arrow.counters.totalRows.load());
    metrics["arrow"]["active_streams"] =
        static_cast<std::int64_t>(arrow.gauges.activeStreams.load());

    // Endpoint inventory, pinned so a concurrent config change cannot
    // race the read.
    if (config_manager) {
        metrics["endpoints"]["count"] =
            static_cast<std::int64_t>(config_manager->endpointsSnapshot()->size());
    }

    metrics["requests"] = RuntimeMetrics::instance().toJson(filter);
    metrics["caches"] = buildCacheMetricsJson(config_manager, filter);

    return metrics;
}


// ============================================================================
// Handler Class Implementations
// ============================================================================

// AuditLogHandler - Already has full implementation above namespace

// FilesystemHandler
FilesystemHandler::FilesystemHandler(std::shared_ptr<ConfigManager> config_manager)
    : config_manager_(config_manager) {}


crow::response FilesystemHandler::getFilesystemStructure(const crow::request& req) {
    try {
        crow::json::wvalue response;
        
        // Get base path and add to response
        auto base_path = config_manager_->getBasePath();
        response["base_path"] = base_path;
        
        // Check if config file exists  
        auto config_file_path = std::filesystem::path(base_path) / "flapi.yaml";
        if (!std::filesystem::exists(config_file_path)) {
            config_file_path = std::filesystem::path(base_path) / "flapi.yml";
        }
        response["config_file_exists"] = std::filesystem::exists(config_file_path);
        response["config_file"] = config_file_path.filename().string();
        
        // Get the template directory path and add to response
        auto template_path = config_manager_->getFullTemplatePath();
        response["template_path"] = template_path.string();
        
        // Build the file tree
        crow::json::wvalue::list tree;
        if (std::filesystem::exists(template_path) && std::filesystem::is_directory(template_path)) {
            buildDirectoryTree(template_path, template_path, tree);
        }
        
        response["tree"] = std::move(tree);
        
        return crow::response(200, response);
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

void FilesystemHandler::buildDirectoryTree(const std::filesystem::path& root_path,
                                           const std::filesystem::path& current_path,
                                           crow::json::wvalue::list& tree) {
    // Collect entries for sorting
    std::vector<std::filesystem::directory_entry> directories;
    std::vector<std::filesystem::directory_entry> files;
    
    for (const auto& entry : std::filesystem::directory_iterator(current_path)) {
        if (entry.is_directory()) {
            directories.push_back(entry);
        } else {
            files.push_back(entry);
        }
    }
    
    // Sort alphabetically
    auto comparator = [](const auto& a, const auto& b) {
        return a.path().filename().string() < b.path().filename().string();
    };
    std::sort(directories.begin(), directories.end(), comparator);
    std::sort(files.begin(), files.end(), comparator);
    
    // Add directories first
    for (const auto& dir_entry : directories) {
        crow::json::wvalue node;
        node["name"] = dir_entry.path().filename().string();
        node["type"] = "directory";
        node["path"] = std::filesystem::relative(dir_entry.path(), root_path).string();
        
        // Recursively build children
        crow::json::wvalue::list children;
        buildDirectoryTree(root_path, dir_entry.path(), children);
        node["children"] = std::move(children);
        
        tree.push_back(std::move(node));
    }
    
    // Then add files
    for (const auto& file_entry : files) {
        crow::json::wvalue node = buildFileNode(file_entry.path(), root_path);
        tree.push_back(std::move(node));
    }
}

crow::json::wvalue FilesystemHandler::buildFileNode(const std::filesystem::path& file_path,
                                                    const std::filesystem::path& root_path) {
    crow::json::wvalue node;
    node["name"] = file_path.filename().string();
    node["type"] = "file";
    node["path"] = std::filesystem::relative(file_path, root_path).string();
    node["extension"] = file_path.extension().string();
    
    // If it's a YAML file, parse it for additional metadata
    if (file_path.extension() == ".yaml" || file_path.extension() == ".yml") {
        try {
            // Use ExtendedYamlParser to support {{include}} directives and environment variables
            flapi::ExtendedYamlParser::IncludeConfig include_config;
            // NO substitution: a file browser shows the file as written. This parser
            // used to have an empty whitelist, which meant "allow everything", so a
            // `url-path: /x-{{env.SECRET}}` put any environment variable's VALUE in
            // the response for whoever held the config-service token (#157). Even a
            // whitelisted value has no business in a directory listing.
            include_config.allow_environment_variables = false;
            include_config.allow_conditional_includes = true;
            
            flapi::ExtendedYamlParser parser(include_config);
            auto parse_result = parser.parseFile(file_path);
            
            if (!parse_result.success) {
                CROW_LOG_WARNING << "Failed to parse YAML file " << file_path << ": " << parse_result.error_message;
                return node;
            }
            
            YAML::Node yaml_content = parse_result.node;
            
            // Determine YAML type
            if (yaml_content["url-path"]) {
                // REST endpoint
                node["yaml_type"] = "endpoint";
                node["url_path"] = yaml_content["url-path"].as<std::string>();
                
                if (yaml_content["template-source"]) {
                    node["template_source"] = yaml_content["template-source"].as<std::string>();
                }
                
                // Check for cache configuration
                if (yaml_content["cache"] && yaml_content["cache"]["enabled"] && 
                    yaml_content["cache"]["enabled"].as<bool>()) {
                    if (yaml_content["cache"]["template-file"]) {
                        node["cache_template_source"] = yaml_content["cache"]["template-file"].as<std::string>();
                    }
                }
            } else if (yaml_content["mcp-tool"]) {
                // MCP tool
                node["yaml_type"] = "mcp-tool";
                if (yaml_content["mcp-tool"]["name"]) {
                    node["mcp_name"] = yaml_content["mcp-tool"]["name"].as<std::string>();
                }
                if (yaml_content["template-source"]) {
                    node["template_source"] = yaml_content["template-source"].as<std::string>();
                }
            } else if (yaml_content["mcp-resource"]) {
                // MCP resource
                node["yaml_type"] = "mcp-resource";
                if (yaml_content["mcp-resource"]["name"]) {
                    node["mcp_name"] = yaml_content["mcp-resource"]["name"].as<std::string>();
                }
                if (yaml_content["template-source"]) {
                    node["template_source"] = yaml_content["template-source"].as<std::string>();
                }
            } else if (yaml_content["mcp-prompt"]) {
                // MCP prompt
                node["yaml_type"] = "mcp-prompt";
                if (yaml_content["mcp-prompt"]["name"]) {
                    node["mcp_name"] = yaml_content["mcp-prompt"]["name"].as<std::string>();
                }
            } else {
                // Shared configuration file
                node["yaml_type"] = "shared";
            }
        } catch (const std::exception& e) {
            // If YAML parsing fails, just mark it as a regular file
            CROW_LOG_WARNING << "Failed to parse YAML file " << file_path << ": " << e.what();
        }
    }
    
    return node;
}

// SchemaHandler
SchemaHandler::SchemaHandler(std::shared_ptr<ConfigManager> config_manager)
    : config_manager_(config_manager) {}

// TemplateHandler
TemplateHandler::TemplateHandler(std::shared_ptr<ConfigManager> config_manager)
    : config_manager_(config_manager) {}

// CacheConfigHandler
CacheConfigHandler::CacheConfigHandler(std::shared_ptr<ConfigManager> config_manager)
    : config_manager_(config_manager) {}

// EndpointConfigHandler
EndpointConfigHandler::EndpointConfigHandler(std::shared_ptr<ConfigManager> config_manager)
    : config_manager_(config_manager) {}

// ProjectConfigHandler
ProjectConfigHandler::ProjectConfigHandler(std::shared_ptr<ConfigManager> config_manager)
    : config_manager_(config_manager) {}

// ============================================================================
// ConfigService Implementation (Facade)
// ============================================================================

ConfigService::ConfigService(std::shared_ptr<ConfigManager> config_manager,
                           bool enabled,
                           const std::string& auth_token)
    : config_manager(config_manager),
      audit_handler_(std::make_unique<AuditLogHandler>(config_manager)),
      filesystem_handler_(std::make_unique<FilesystemHandler>(config_manager)),
      schema_handler_(std::make_unique<SchemaHandler>(config_manager)),
      template_handler_(std::make_unique<TemplateHandler>(config_manager)),
      cache_handler_(std::make_unique<CacheConfigHandler>(config_manager)),
      endpoint_handler_(std::make_unique<EndpointConfigHandler>(config_manager)),
      project_handler_(std::make_unique<ProjectConfigHandler>(config_manager)),
      doc_generator_(nullptr),
      enabled_(enabled),
      auth_token_(auth_token) {
    // A short token is accepted - REST and the flapi_* MCP tools must agree on
    // what authenticates, and refusing it on one transport only (as the MCP
    // tools did below 8 characters) just broke that tool set. Say it is weak
    // once, where the operator chose it.
    if (enabled_ && !auth_token_.empty() && auth_token_.size() < kMinRecommendedTokenLength) {
        CROW_LOG_WARNING << "The config-service token is only " << auth_token_.size()
                         << " characters long; use at least " << kMinRecommendedTokenLength
                         << " (omit --config-service-token to have a strong one generated)";
    }
}

namespace {

// Every config-service 401 says the same thing in the same shape (JSON, like
// every other error here). They used to differ by route: plain text on most,
// {"error":"Unauthorized"} on /metrics.
// The database was invalidated by an internal DuckDB error (see DatabaseHealth):
// every route that would touch it answers this instead of a 500 per request.
crow::response databaseFatalResponse() {
    crow::response res(503, DatabaseHealth::FatalResponseJson());
    res.set_header("Content-Type", "application/json");
    res.set_header("Retry-After", "30");
    return res;
}

crow::response unauthorizedResponse() {
    crow::json::wvalue body;
    body["error"] = "Unauthorized: missing or invalid config-service token "
                    "(send it as X-Config-Token or Authorization: Bearer)";
    crow::response res(401, body);
    res.set_header("Content-Type", "application/json");
    return res;
}

crow::response slugError(int status, const std::string& message) {
    crow::json::wvalue body;
    body["error"] = message;
    crow::response res(status, body);
    res.set_header("Content-Type", "application/json");
    return res;
}

std::string upperCopy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

}  // namespace

std::string DecodeSlugSegment(const std::string& slug) {
    // ':' is legal in a path segment (RFC 3986 pchar), but encodeURIComponent
    // sends it as %3A, and the slug of /products/:id arrived as "-products-%3Aid"
    // and matched nothing. Only %3A is decoded: '%' never occurs in a slug
    // (see path_utils.cpp), so this cannot alias another endpoint.
    std::string out;
    out.reserve(slug.size());
    for (std::size_t i = 0; i < slug.size(); ++i) {
        if (slug[i] == '%' && i + 3 <= slug.size() &&
            (slug.compare(i, 3, "%3A") == 0 || slug.compare(i, 3, "%3a") == 0)) {
            out += ':';
            i += 2;
        } else {
            out += slug[i];
        }
    }
    return out;
}

SlugTarget ResolveSlugTarget(const ConfigManager& cm, const std::string& raw_slug, const char* method_param,
                             bool cached_only) {
    SlugTarget target;
    const std::string slug = DecodeSlugSegment(raw_slug);
    const std::string path = PathUtils::identifierToPath(slug);
    const std::string method = method_param != nullptr ? upperCopy(method_param) : std::string();

    // Exact names first (the slug, the url-path, the MCP name); a route match
    // (/orders/7 against /orders/:id) only when nothing is named exactly.
    std::vector<const EndpointConfig*> exact;
    std::vector<const EndpointConfig*> matched;
    const auto snapshot = cm.getEndpoints();
    for (const auto& ep : *snapshot) {
        if (!method.empty()) {
            // ?method= names a REST endpoint.
            if (ep.urlPath.empty() || upperCopy(ep.method.empty() ? "GET" : ep.method) != method) {
                continue;
            }
        }
        const bool is_exact = ep.getSlug() == slug || (!ep.urlPath.empty() && ep.urlPath == path) ||
                              (ep.urlPath.empty() && ep.getName() == slug);
        if (is_exact) {
            exact.push_back(&ep);
        } else if (ep.matchesPath(path)) {
            matched.push_back(&ep);
        }
    }
    auto& candidates = exact.empty() ? matched : exact;
    // Method order, so the "ambiguous" message is stable across file systems.
    std::sort(candidates.begin(), candidates.end(), [](const EndpointConfig* a, const EndpointConfig* b) {
        return ConfigManager::EndpointKey(*a) < ConfigManager::EndpointKey(*b);
    });
    if (cached_only && candidates.size() > 1) {
        // A /cache/* route is about a cache: a cached GET /orders beside an
        // uncached POST /orders is not ambiguous. Only endpoints that have a
        // cache count, and several of them on ONE table are one cache. With
        // none cached, the first is kept and the route says it has no cache.
        std::vector<const EndpointConfig*> cached;
        for (const auto* ep : candidates) {
            if (ep->cache.enabled && !ep->cache.table.empty()) {
                cached.push_back(ep);
            }
        }
        const auto same_table = [&cached]() {
            for (const auto* ep : cached) {
                if (ep->cache.table != cached.front()->cache.table ||
                    ep->cache.schema != cached.front()->cache.schema) {
                    return false;
                }
            }
            return true;
        };
        if (cached.empty()) {
            candidates.resize(1);
        } else if (same_table()) {
            // The one naming a populate template, as warm-up picks.
            const auto with_template = std::find_if(cached.begin(), cached.end(),
                                                    [](const EndpointConfig* ep) { return ep->cache.hasTemplate(); });
            candidates = {with_template != cached.end() ? *with_template : cached.front()};
        } else {
            candidates = cached;
        }
    }
    target.found = !candidates.empty();
    if (candidates.empty()) {
        target.status = 404;
        target.error = "Endpoint not found: " + (path.empty() ? slug : path) +
                       (method.empty() ? std::string() : " with method " + method);
        return target;
    }
    if (candidates.size() > 1) {
        // Several endpoints share the url-path (GET /orders + POST /orders).
        // Picking one silently is how reload reloaded the other file and
        // answered 200; say so and let the caller name it.
        std::string served;
        std::string methods;
        for (const auto* ep : candidates) {
            const std::string m = upperCopy(ep->method.empty() ? "GET" : ep->method);
            served += (served.empty() ? "" : ", ") + m + " (" +
                      (ep->config_file_path.empty() ? std::string("in memory") : ep->config_file_path) + ")";
            methods += (methods.empty() ? "" : "|") + m;
        }
        target.status = 400;
        target.error = "Ambiguous: " + (path.empty() ? slug : path) + " is served by " + served +
                       "; specify ?method=" + methods;
        return target;
    }
    target.ok = true;
    target.status = 200;
    target.key = ConfigManager::EndpointKey(*candidates.front());
    return target;
}

namespace {

// The route-level form: resolved, or the JSON error to answer with.
std::optional<crow::response> resolveOrError(const ConfigManager& cm, const std::string& slug,
                                             const crow::request& req, std::string& key,
                                             bool cached_only = false) {
    const auto target = ResolveSlugTarget(cm, slug, req.url_params.get("method"), cached_only);
    if (!target.ok) {
        return slugError(target.status, target.error);
    }
    key = target.key;
    return std::nullopt;
}

}  // namespace

void ConfigService::setDocGenerator(std::shared_ptr<OpenAPIDocGenerator> doc_gen) {
    doc_generator_ = doc_gen;
}

std::string ConfigService::RouteTemplateFor(const std::string& url) {
    // The route TEMPLATE for a config-service URL, for the http.route span
    // attribute and span name. Only ever applied to a request a config route
    // matched, so the set of values is the fixed route list below; the one
    // variable segment, the endpoint slug, is replaced by its placeholder so
    // no filled path is ever exported.
    std::string path = url.substr(0, url.find('?'));
    static const std::string endpoints_prefix = "/api/v1/_config/endpoints/";
    if (path.rfind(endpoints_prefix, 0) == 0) {
        const std::string rest = path.substr(endpoints_prefix.size());
        const auto slash = rest.find('/');
        const std::string slug = rest.substr(0, slash);
        if (slug != "by-template" && slug != "reload-file" && !slug.empty()) {
            path = endpoints_prefix + "<string>" +
                   (slash == std::string::npos ? std::string() : rest.substr(slash));
        }
    }
    return path;
}

bool ConfigService::validateToken(const crow::request& req) const {
    // Every config route calls this first, so it is where the route gets its
    // name: the middleware does not resolve routes, and without this every
    // config-service span was labelled "POST <unmatched>" - the bucket a
    // scanner's 404s land in.
    if (auto* rc = RequestContextScope::current(); rc != nullptr) {
        rc->route_template_storage = RouteTemplateFor(req.url);
        rc->route_template = rc->route_template_storage;
    }

    // If service is not enabled, reject all requests
    if (!enabled_) {
        return false;
    }

    // Fail closed with no configured secret.
    //
    // `token == auth_token_` with both empty returns TRUE, so an empty
    // configured token would authenticate `Authorization: Bearer `. That is
    // NOT reachable through the CLI - main.cpp generates a secure token when
    // --config-service is given without one - so this is defence in depth
    // rather than a closed hole: ConfigService can be constructed with an
    // empty token programmatically, and its MCP twin tokenMatchesConfigured
    // was written to fail closed for the same reason.
    if (auth_token_.empty()) {
        return false;
    }

    const auto matches = [this](const std::string& presented) {
        // Constant time, so the comparison cannot be turned into an oracle
        // that reveals the token a character at a time. Length is compared
        // first and does leak the length; CRYPTO_memcmp needs equal sizes.
        if (presented.size() != auth_token_.size()) {
            return false;
        }
        return CRYPTO_memcmp(presented.data(), auth_token_.data(),
                             presented.size()) == 0;
    };

    // A presented-but-wrong token is logged (never the token itself): without
    // this a misconfigured client and a scanner both left no trace beyond 401s.
    const auto rejected = [&req](const char* header) {
        CROW_LOG_WARNING << "Config service: rejected " << header << " token for "
                         << req.url.substr(0, req.url.find('?'));
        return false;
    };

    // Check Authorization header: "Bearer <token>"
    auto auth_header = req.get_header_value("Authorization");
    if (!auth_header.empty()) {
        const std::string bearer_prefix = "Bearer ";
        if (auth_header.substr(0, bearer_prefix.length()) == bearer_prefix) {
            return matches(auth_header.substr(bearer_prefix.length())) || rejected("Authorization: Bearer");
        }
    }

    // Also check X-Config-Token header for easier testing
    auto token_header = req.get_header_value("X-Config-Token");
    if (!token_header.empty()) {
        return matches(token_header) || rejected("X-Config-Token");
    }

    return false;
}

void ConfigService::registerRoutes(FlapiApp& app) {
    CROW_LOG_INFO << "Registering config routes";
    
    // If config service is not enabled, don't register routes
    if (!enabled_) {
        CROW_LOG_INFO << "Config service is disabled, skipping route registration";
        return;
    }

    // Project configuration routes
    CROW_ROUTE(app, "/api/v1/_config/project")
        .methods("GET"_method, "PUT"_method)
        ([this](const crow::request& req) {
            if (!validateToken(req)) {
                return unauthorizedResponse();
            }
            if (DatabaseHealth::IsFatal()) {
                return databaseFatalResponse();
            }
            if (req.method == crow::HTTPMethod::Get) {
                return project_handler_->getProjectConfig(req);
            }
            // The project document is read-only over this API (edit flapi.yaml
            // and restart). This answered 501 "Not implemented", which reads as
            // "coming soon" and is a 5xx a client may retry; 405 + Allow says
            // the method is not supported on this resource.
            crow::response res(405, "Method Not Allowed: the project configuration is read-only; edit flapi.yaml");
            res.set_header("Allow", "GET");
            return res;
        });

    // Environment variables route
    CROW_ROUTE(app, "/api/v1/_config/environment-variables")
        .methods("GET"_method)
        ([this](const crow::request& req) {
            if (!validateToken(req)) {
                return unauthorizedResponse();
            }
            if (DatabaseHealth::IsFatal()) {
                return databaseFatalResponse();
            }
            return project_handler_->getEnvironmentVariables(req);
        });

    // Endpoints configuration routes
    CROW_ROUTE(app, "/api/v1/_config/endpoints")
        .methods("GET"_method, "POST"_method)
        ([this](const crow::request& req) {
            if (!validateToken(req)) {
                return unauthorizedResponse();
            }
            if (DatabaseHealth::IsFatal()) {
                return databaseFatalResponse();
            }
            if (req.method == crow::HTTPMethod::Get)
                return endpoint_handler_->listEndpoints(req);
            else
                return endpoint_handler_->createEndpoint(req);
        });

    // Reload (or load) every endpoint a YAML file declares. Addressed by FILE,
    // not slug: a new hand-written endpoint has no slug the server knows yet
    // (create answered 409 "declared in file", reload 404), and two files may
    // share a url-path. Registered before the <string> routes it shadows.
    CROW_ROUTE(app, "/api/v1/_config/endpoints/reload-file")
        .methods("POST"_method)
        ([this](const crow::request& req) {
            if (!validateToken(req)) {
                return unauthorizedResponse();
            }
            if (DatabaseHealth::IsFatal()) {
                return databaseFatalResponse();
            }
            return endpoint_handler_->reloadEndpointFile(req);
        });

    // Every slug route below takes an optional ?method=: several endpoints may
    // share a url-path, and without it such a slug is a 400, not a guess.
    CROW_ROUTE(app, "/api/v1/_config/endpoints/<string>")
        .methods("GET"_method, "PUT"_method, "DELETE"_method)
        ([this](const crow::request& req, const std::string& slug) {
            if (!validateToken(req)) {
                return unauthorizedResponse();
            }
            if (DatabaseHealth::IsFatal()) {
                return databaseFatalResponse();
            }
            std::string key;
            if (auto error = resolveOrError(*config_manager, slug, req, key)) {
                return std::move(*error);
            }
            switch (req.method) {
                case crow::HTTPMethod::Get:
                    return endpoint_handler_->getEndpointConfig(req, key);
                case crow::HTTPMethod::Put:
                    return endpoint_handler_->updateEndpointConfig(req, key);
                case crow::HTTPMethod::Delete:
                    return endpoint_handler_->deleteEndpoint(req, key);
                default:
                    return crow::response(405);
            }
        });

    // Endpoint validation and reload routes
    CROW_ROUTE(app, "/api/v1/_config/endpoints/<string>/validate")
        .methods("POST"_method)
        ([this](const crow::request& req, const std::string& slug) {
            if (!validateToken(req)) {
                return unauthorizedResponse();
            }
            if (DatabaseHealth::IsFatal()) {
                return databaseFatalResponse();
            }
            // Validating a not-yet-existing endpoint is fine: the target only
            // chooses the directory the YAML is checked from.
            const auto target = ResolveSlugTarget(*config_manager, slug, req.url_params.get("method"));
            if (!target.ok && target.found) {
                return slugError(target.status, target.error);
            }
            return endpoint_handler_->validateEndpointConfig(
                req, target.ok ? target.key : PathUtils::identifierToPath(DecodeSlugSegment(slug)));
        });

    CROW_ROUTE(app, "/api/v1/_config/endpoints/<string>/reload")
        .methods("POST"_method)
        ([this](const crow::request& req, const std::string& slug) {
            if (!validateToken(req)) {
                return unauthorizedResponse();
            }
            if (DatabaseHealth::IsFatal()) {
                return databaseFatalResponse();
            }
            std::string key;
            if (auto error = resolveOrError(*config_manager, slug, req, key)) {
                return std::move(*error);
            }
            return endpoint_handler_->reloadEndpointConfig(req, key);
        });

    // Parameters route
    CROW_ROUTE(app, "/api/v1/_config/endpoints/<string>/parameters")
        .methods("GET"_method)
        ([this](const crow::request& req, const std::string& slug) {
            if (!validateToken(req)) {
                return unauthorizedResponse();
            }
            if (DatabaseHealth::IsFatal()) {
                return databaseFatalResponse();
            }
            std::string key;
            if (auto error = resolveOrError(*config_manager, slug, req, key)) {
                return std::move(*error);
            }
            return endpoint_handler_->getEndpointParameters(req, key);
        });

    // Find endpoints by template file
    CROW_ROUTE(app, "/api/v1/_config/endpoints/by-template")
        .methods("POST"_method)
        ([this](const crow::request& req) {
            if (!validateToken(req)) {
                return unauthorizedResponse();
            }
            if (DatabaseHealth::IsFatal()) {
                return databaseFatalResponse();
            }
            auto json = crow::json::load(req.body);
            if (!json || !json.has("template_path")) {
                return slugError(400, "Invalid JSON: missing 'template_path' field");
            }
            const std::string template_path = json["template_path"].s();
            return endpoint_handler_->findEndpointsByTemplate(req, template_path);
        });

    // Template routes
    CROW_ROUTE(app, "/api/v1/_config/endpoints/<string>/template")
        .methods("GET"_method, "PUT"_method)
        ([this](const crow::request& req, const std::string& slug) {
            if (!validateToken(req)) {
                return unauthorizedResponse();
            }
            if (DatabaseHealth::IsFatal()) {
                return databaseFatalResponse();
            }
            std::string key;
            if (auto error = resolveOrError(*config_manager, slug, req, key)) {
                return std::move(*error);
            }
            if (req.method == crow::HTTPMethod::Get)
                return template_handler_->getEndpointTemplate(req, key);
            else
                return template_handler_->updateEndpointTemplate(req, key);
        });

    CROW_ROUTE(app, "/api/v1/_config/endpoints/<string>/template/expand")
        .methods("POST"_method)
        ([this](const crow::request& req, const std::string& slug) {
            if (!validateToken(req)) {
                return unauthorizedResponse();
            }
            if (DatabaseHealth::IsFatal()) {
                return databaseFatalResponse();
            }
            std::string key;
            if (auto error = resolveOrError(*config_manager, slug, req, key)) {
                return std::move(*error);
            }
            return template_handler_->expandTemplate(req, key);
        });

    CROW_ROUTE(app, "/api/v1/_config/endpoints/<string>/template/test")
        .methods("POST"_method)
        ([this](const crow::request& req, const std::string& slug) {
            if (!validateToken(req)) {
                return unauthorizedResponse();
            }
            if (DatabaseHealth::IsFatal()) {
                return databaseFatalResponse();
            }
            std::string key;
            if (auto error = resolveOrError(*config_manager, slug, req, key)) {
                return std::move(*error);
            }
            return template_handler_->testTemplate(req, key);
        });

    // Cache routes
    CROW_ROUTE(app, "/api/v1/_config/endpoints/<string>/cache")
        .methods("GET"_method, "PUT"_method)
        ([this](const crow::request& req, const std::string& slug) {
            if (!validateToken(req)) {
                return unauthorizedResponse();
            }
            if (DatabaseHealth::IsFatal()) {
                return databaseFatalResponse();
            }
            std::string key;
            if (auto error = resolveOrError(*config_manager, slug, req, key, /*cached_only=*/true)) {
                return std::move(*error);
            }
            if (req.method == crow::HTTPMethod::Get)
                return cache_handler_->getCacheConfig(req, key);
            else
                return cache_handler_->updateCacheConfig(req, key);
        });

    CROW_ROUTE(app, "/api/v1/_config/endpoints/<string>/cache/template")
        .methods("GET"_method, "PUT"_method)
        ([this](const crow::request& req, const std::string& slug) {
            if (!validateToken(req)) {
                return unauthorizedResponse();
            }
            if (DatabaseHealth::IsFatal()) {
                return databaseFatalResponse();
            }
            std::string key;
            if (auto error = resolveOrError(*config_manager, slug, req, key, /*cached_only=*/true)) {
                return std::move(*error);
            }
            if (req.method == crow::HTTPMethod::Get)
                return template_handler_->getCacheTemplate(req, key);
            else
                return template_handler_->updateCacheTemplate(req, key);
        });

    CROW_ROUTE(app, "/api/v1/_config/endpoints/<string>/cache/refresh")
        .methods("POST"_method)
        ([this](const crow::request& req, const std::string& slug) {
            if (!validateToken(req)) {
                return unauthorizedResponse();
            }
            if (DatabaseHealth::IsFatal()) {
                return databaseFatalResponse();
            }
            std::string key;
            if (auto error = resolveOrError(*config_manager, slug, req, key, /*cached_only=*/true)) {
                return std::move(*error);
            }
            return cache_handler_->refreshCache(req, key);
        });

    CROW_ROUTE(app, "/api/v1/_config/endpoints/<string>/cache/gc")
        .methods("POST"_method)
        ([this](const crow::request& req, const std::string& slug) {
            if (!validateToken(req)) {
                return unauthorizedResponse();
            }
            if (DatabaseHealth::IsFatal()) {
                return databaseFatalResponse();
            }
            std::string key;
            if (auto error = resolveOrError(*config_manager, slug, req, key, /*cached_only=*/true)) {
                return std::move(*error);
            }
            return cache_handler_->performGarbageCollection(req, key);
        });

    // Drop an obsolete cache table (left behind by a changed `schema:`/`table:`):
    // its snapshots are expired and its files deleted too.
    CROW_ROUTE(app, "/api/v1/_config/cache/drop-table")
        .methods("POST"_method)
        ([this](const crow::request& req) {
            if (!validateToken(req)) {
                return unauthorizedResponse();
            }
            if (DatabaseHealth::IsFatal()) {
                return databaseFatalResponse();
            }
            return cache_handler_->dropCacheTable(req);
        });

    // Run DuckLake compaction now - catalog-wide, or for one endpoint's table
    // ({"endpoint": "/orders", "method": "GET"}). It ran only on its schedule, so
    // an operator who saw files pile up had no way to act before the next run.
    CROW_ROUTE(app, "/api/v1/_config/cache/compact")
        .methods("POST"_method)
        ([this](const crow::request& req) {
            if (!validateToken(req)) {
                return unauthorizedResponse();
            }
            if (DatabaseHealth::IsFatal()) {
                return databaseFatalResponse();
            }
            return cache_handler_->compactCache(req);
        });

    // DuckLake audit routes
    CROW_ROUTE(app, "/api/v1/_config/endpoints/<string>/cache/audit")
        .methods("GET"_method)
        ([this](const crow::request& req, const std::string& slug) {
            if (!validateToken(req)) {
                return unauthorizedResponse();
            }
            if (DatabaseHealth::IsFatal()) {
                return databaseFatalResponse();
            }
            std::string key;
            if (auto error = resolveOrError(*config_manager, slug, req, key, /*cached_only=*/true)) {
                return std::move(*error);
            }
            return audit_handler_->getCacheAuditLog(key);
        });

CROW_ROUTE(app, "/api/v1/_config/cache/audit")
        .methods("GET"_method)
        ([this](const crow::request& req) {
            if (!validateToken(req)) {
                return unauthorizedResponse();
            }
            if (DatabaseHealth::IsFatal()) {
                return databaseFatalResponse();
            }
            const char* table = req.url_params.get("table");
            const char* sync_type = req.url_params.get("sync_type");
            return audit_handler_->getAllCacheAuditLogs(table != nullptr ? table : "",
                                                        sync_type != nullptr ? sync_type : "");
        });

    // Schema routes
    CROW_ROUTE(app, "/api/v1/_config/schema")
        .methods("GET"_method)
        ([this](const crow::request& req) {
            if (!validateToken(req)) {
                return unauthorizedResponse();
            }
            if (DatabaseHealth::IsFatal()) {
                return databaseFatalResponse();
            }
            return schema_handler_->getSchema(req);
        });

    CROW_ROUTE(app, "/api/v1/_config/schema/refresh")
        .methods("POST"_method)
        ([this](const crow::request& req) {
            if (!validateToken(req)) {
                return unauthorizedResponse();
            }
            if (DatabaseHealth::IsFatal()) {
                return databaseFatalResponse();
            }
            return schema_handler_->refreshSchema(req);
        });

    // Filesystem routes
    CROW_ROUTE(app, "/api/v1/_config/filesystem")
        .methods("GET"_method)
        ([this](const crow::request& req) {
            if (!validateToken(req)) {
                return unauthorizedResponse();
            }
            if (DatabaseHealth::IsFatal()) {
                return databaseFatalResponse();
            }
            return filesystem_handler_->getFilesystemStructure(req);
        });
    
    // Log level control routes
    CROW_ROUTE(app, "/api/v1/_config/log-level")
        .methods("GET"_method, "PUT"_method)
        ([this](const crow::request& req) {
            if (!validateToken(req)) {
                return unauthorizedResponse();
            }
            
            if (req.method == crow::HTTPMethod::Get) {
                // Get current log level
                crow::json::wvalue response;
                response["level"] = current_log_level_;
                return crow::response(200, response);
            } else {
                // Set new log level
                auto json = crow::json::load(req.body);
                if (!json || !json.has("level")) {
                    return crow::response(400, "Missing 'level' field");
                }
                
                std::string level_str = json["level"].s();
                std::transform(level_str.begin(), level_str.end(), level_str.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                // `warn` is what --log-level, FLAPI_LOG_LEVEL and `log-level:` accept;
                // this was the one path that refused it.
                if (level_str == "warn") {
                    level_str = "warning";
                }

                // Validate and set log level
                if (level_str == "debug") {
                    crow::logger::setLogLevel(crow::LogLevel::Debug);
                    current_log_level_ = "debug";
                } else if (level_str == "info") {
                    crow::logger::setLogLevel(crow::LogLevel::Info);
                    current_log_level_ = "info";
                } else if (level_str == "warning") {
                    crow::logger::setLogLevel(crow::LogLevel::Warning);
                    current_log_level_ = "warning";
                } else if (level_str == "error") {
                    crow::logger::setLogLevel(crow::LogLevel::Error);
                    current_log_level_ = "error";
                } else {
                    return crow::response(400, "Invalid log level. Use: debug, info, warning (or warn), error");
                }
                
                CROW_LOG_INFO << "Log level changed to: " << current_log_level_;
                
                crow::json::wvalue response;
                response["level"] = current_log_level_;
                response["message"] = "Log level updated successfully";
                return crow::response(200, response);
            }
        });
    
    // Metrics endpoint.
    //
    // This has been published in the OpenAPI document since
    // open_api_doc_generator.cpp:563 with no route behind it - a documented
    // endpoint that 404s. Implemented rather than deleted because the tracing
    // drop/export counters need a surface anyway: NFR-4 requires that an
    // unreachable collector be observable as dropped spans rather than as silent
    // data loss.
    //
    // Bearer-gated like its siblings: it reports internal operational state.
    CROW_ROUTE(app, "/api/v1/_config/metrics")
        .methods("GET"_method)
        ([this](const crow::request& req) {
            if (!validateToken(req)) {
                return unauthorizedResponse();
            }

            // ?endpoint=/path narrows the requests and caches blocks to one route / tool.
            const char* filter_param = req.url_params.get("endpoint");
            crow::json::wvalue metrics = buildMetricsJson(config_manager, filter_param ? filter_param : "");

            // dumpJson, not dump(): crow prints doubles with 21 significant
            // digits, so a rounded 7.496 came out as 7.49600000000000044.
            crow::response res(200, QueryResult::dumpJson(metrics));
            res.set_header("Content-Type", "application/json");
            return res;
        });

    // Health check endpoint (no authentication required)
    CROW_ROUTE(app, "/api/v1/_config/health")
        .methods("GET"_method)
        ([this](const crow::request& req) {
            crow::json::wvalue health;

            // Server status
            health["status"] = "healthy";
            health["server"]["name"] = "flapi";
            health["server"]["version"] = FLAPI_VERSION;

            // Database status
            auto db_manager = DatabaseManager::getInstance();
            health["database"]["status"] = db_manager ? "connected" : "disconnected";
            const bool database_fatal = DatabaseHealth::IsFatal();
            if (database_fatal) {
                // Invalidated by an internal error: nothing works until a restart.
                health["status"] = "fatal";
                health["database"]["status"] = "invalidated";
                health["database"]["reason"] = DatabaseHealth::FatalReason().value_or("");
            }

            // Endpoints count
            if (config_manager) {
                health["endpoints"]["count"] = static_cast<int>(config_manager->getEndpoints()->size());
            }

            // Arrow IPC status
            auto& arrowMetrics = ArrowMetrics::instance();
            crow::json::wvalue arrow;
            arrow["enabled"] = true;
            arrow["codecs"] = crow::json::wvalue::list({"lz4", "zstd"});

            // Arrow counters
            crow::json::wvalue counters;
            counters["total_requests"] = arrowMetrics.counters.totalRequests.load();
            counters["successful_requests"] = arrowMetrics.counters.successfulRequests.load();
            counters["failed_requests"] = arrowMetrics.counters.failedRequests.load();
            counters["total_rows"] = arrowMetrics.counters.totalRows.load();
            counters["total_batches"] = arrowMetrics.counters.totalBatches.load();
            counters["total_bytes_written"] = arrowMetrics.counters.totalBytesWritten.load();
            counters["total_bytes_compressed"] = arrowMetrics.counters.totalBytesCompressed.load();
            counters["compression_requests"] = arrowMetrics.counters.compressionRequests.load();
            arrow["counters"] = std::move(counters);

            // Arrow gauges
            crow::json::wvalue gauges;
            gauges["active_streams"] = arrowMetrics.gauges.activeStreams.load();
            gauges["peak_active_streams"] = arrowMetrics.gauges.peakActiveStreams.load();
            gauges["current_memory_bytes"] = arrowMetrics.gauges.currentMemoryUsage.load();
            gauges["peak_memory_bytes"] = arrowMetrics.gauges.peakMemoryUsage.load();
            arrow["gauges"] = std::move(gauges);

            // Arrow histograms (calculated values)
            crow::json::wvalue stats;
            stats["avg_duration_us"] = arrowMetrics.getAverageDurationUs();
            stats["avg_compression_ratio"] = arrowMetrics.getAverageCompressionRatio();
            uint64_t minDuration = arrowMetrics.histograms.minDurationUs.load();
            uint64_t maxDuration = arrowMetrics.histograms.maxDurationUs.load();
            if (minDuration != UINT64_MAX) {
                stats["min_duration_us"] = minDuration;
            }
            if (maxDuration > 0) {
                stats["max_duration_us"] = maxDuration;
            }
            uint64_t minBatch = arrowMetrics.histograms.minBatchRows.load();
            uint64_t maxBatch = arrowMetrics.histograms.maxBatchRows.load();
            if (minBatch != UINT64_MAX) {
                stats["min_batch_rows"] = minBatch;
            }
            if (maxBatch > 0) {
                stats["max_batch_rows"] = maxBatch;
            }
            arrow["stats"] = std::move(stats);

            health["arrow"] = std::move(arrow);

            // Storage health status
            if (config_manager) {
                VFSHealthChecker vfs_checker;
                std::string config_path = config_manager->getBasePath();
                std::string templates_path = config_manager->getTemplatePath();

                auto storage_health = vfs_checker.checkHealth(config_path, templates_path);

                crow::json::wvalue storage;
                storage["status"] = storage_health.healthy ? "healthy" : "unhealthy";
                storage["total_latency_ms"] = storage_health.total_latency_ms;

                // `path` and the raw `error` are withheld here.
                //
                // This is the only _config route without validateToken, and a
                // storage path can carry inline credentials or a signature -
                // which is exactly why DuckLake's metadata-path and data-path
                // were added to the secret collector. An unauthenticated
                // caller asking "is storage healthy?" needs the answer, not
                // the location, and the full detail is a token away at
                // GET /api/v1/_config/project.
                const bool authenticated = validateToken(req);
                crow::json::wvalue backends;
                for (const auto& backend : storage_health.backends) {
                    crow::json::wvalue backend_info;
                    backend_info["accessible"] = backend.accessible;
                    backend_info["latency_ms"] = backend.latency_ms;
                    backend_info["scheme"] = backend.scheme;
                    if (authenticated) {
                        backend_info["path"] = backend.path;
                        if (!backend.error.empty()) {
                            backend_info["error"] = backend.error;
                        }
                    } else if (!backend.error.empty()) {
                        backend_info["error"] = "unavailable; authenticate for details";
                    }
                    backends[backend.name] = std::move(backend_info);
                }
                storage["backends"] = std::move(backends);

                health["storage"] = std::move(storage);
            }

            // Credential status (without revealing secrets)
            {
                auto& cred_manager = getGlobalCredentialManager();
                crow::json::wvalue credentials;
                credentials["s3_configured"] = cred_manager.hasS3Credentials();
                credentials["gcs_configured"] = cred_manager.hasGCSCredentials();
                credentials["azure_configured"] = cred_manager.hasAzureCredentials();
                health["credentials"] = std::move(credentials);
            }

            return crow::response(database_fatal ? 503 : 200, health);
        });

    // OpenAPI documentation route
    CROW_ROUTE(app, "/api/v1/doc.yaml")
        .methods("GET"_method)
        ([this, &app](const crow::request& req) {
            if (!doc_generator_) {
                return crow::response(503, "Documentation generator not initialized");
            }
            
            try {
                YAML::Node doc = doc_generator_->generateConfigServiceDoc(const_cast<FlapiApp&>(app));
                std::stringstream ss;
                ss << doc;
                
                crow::response resp(200, ss.str());
                resp.set_header("Content-Type", "application/x-yaml");
                resp.set_header("Content-Disposition", "inline; filename=\"flapi-config-api.yaml\"");
                return resp;
            } catch (const std::exception& e) {
                CROW_LOG_ERROR << "Error generating OpenAPI documentation: " << e.what();
                return crow::response(500, std::string("Error generating documentation: ") + e.what());
            }
        });
}

// ============================================================================
// AuditLogHandler Implementation
// ============================================================================

AuditLogHandler::AuditLogHandler(std::shared_ptr<ConfigManager> config_manager)
    : config_manager_(config_manager) {}

namespace {
// The audit query, falling back for a catalog whose audit table predates the
// `full` column - a read-only replica reading a catalog its (older) writer has
// not migrated yet. Only the writer adds the column (initializeAuditTables).
QueryResult runAuditQuery(DatabaseManager& db_manager, const std::string& query) {
    std::map<std::string, std::string> params;
    try {
        return db_manager.executeDuckLakeQuery(query, params);
    } catch (const std::exception& e) {
        static const std::string with_full = "coalesce(\"full\", false) AS \"full\"";
        const auto at = query.find(with_full);
        if (at == std::string::npos || std::string(e.what()).find("full") == std::string::npos) {
            throw;
        }
        std::string legacy = query;
        legacy.replace(at, with_full.size(), "false AS \"full\"");
        return db_manager.executeDuckLakeQuery(legacy, params);
    }
}
}  // namespace

crow::response AuditLogHandler::getCacheAuditLog(const std::string& path) {
    try {
        const auto endpoint = config_manager_->getEndpointForKey(path);
        if (!endpoint) {
            return crow::response(404, "Endpoint not found");
        }

        if (!endpoint->cache.enabled) {
            return crow::response(400, "Cache not enabled for this endpoint");
        }

        const auto& ducklake_config = config_manager_->getDuckLakeConfig();
        if (!ducklake_config.enabled) {
            return crow::response(400, "DuckLake not enabled");
        }

        auto db_manager = DatabaseManager::getInstance();
        const std::string catalog = ducklake_config.alias;
        
        // Filter on the endpoint's own url-path, which is what each sync event
        // records. The caller's path only RESOLVES to the endpoint: for
        // /customers/:id it may arrive as /customers/%3Aid or /customers/123,
        // and filtering on that matched no rows - an empty audit for every
        // endpoint with a path parameter.
        std::string audit_query = buildAuditQuery(catalog, endpoint->urlPath);
        return crow::response(200, runAuditQuery(*db_manager, audit_query).data);
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

crow::response AuditLogHandler::getAllCacheAuditLogs(const std::string& table, const std::string& sync_type) {
    try {
        const auto& ducklake_config = config_manager_->getDuckLakeConfig();
        if (!ducklake_config.enabled) {
            return crow::response(400, "DuckLake not enabled");
        }

        auto db_manager = DatabaseManager::getInstance();
        const std::string catalog = ducklake_config.alias;
        
        std::string audit_query = buildAuditQuery(catalog, "", table, sync_type);
        return crow::response(200, runAuditQuery(*db_manager, audit_query).data);
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}


std::string AuditLogHandler::buildAuditQuery(const std::string& catalog,
                                               const std::string& endpoint_filter,
                                               const std::string& table_filter,
                                               const std::string& sync_type_filter) const {
    std::ostringstream query;
    query << R"(
        SELECT event_id, endpoint_path, cache_table, cache_schema, sync_type, status, message,
               snapshot_id, rows_affected, sync_started_at, sync_completed_at, duration_ms,
               coalesce("full", false) AS "full"
        FROM )" << catalog << R"(.audit.sync_events)";
    
    if (!endpoint_filter.empty()) {
        // Quote-doubled. The caller's endpoint path reaches here, and a
        // parameterised endpoint like /orders/:id matches
        // "/orders/x' OR '1'='1", so this was an injection point into the
        // DuckLake audit query.
        std::string escaped_filter;
        escaped_filter.reserve(endpoint_filter.size());
        for (const char c : endpoint_filter) {
            if (c == '\'') {
                escaped_filter += "''";
            } else {
                escaped_filter += c;
            }
        }
        query << "\n        WHERE endpoint_path = '" << escaped_filter << "'";
    } else if (!table_filter.empty() || !sync_type_filter.empty()) {
        // Per cache TABLE: several endpoints can share one, and a refresh is
        // filed under whichever endpoint triggered it - the per-endpoint view
        // showed only part of that table's history.
        const auto quote = [](const std::string& v) {
            std::string out = "'";
            for (const char c : v) {
                out += c == '\'' ? std::string("''") : std::string(1, c);
            }
            return out + "'";
        };
        std::vector<std::string> conditions;
        if (!table_filter.empty()) {
            const auto dot = table_filter.find('.');
            if (dot == std::string::npos) {
                conditions.push_back("cache_table = " + quote(table_filter));
            } else {
                conditions.push_back("cache_schema = " + quote(table_filter.substr(0, dot)) +
                                     " AND cache_table = " + quote(table_filter.substr(dot + 1)));
            }
        }
        if (!sync_type_filter.empty()) {
            conditions.push_back("sync_type = " + quote(sync_type_filter));
        }
        for (std::size_t i = 0; i < conditions.size(); ++i) {
            query << (i == 0 ? "\n        WHERE " : " AND ") << conditions[i];
        }
    }
    
    query << R"(
        ORDER BY sync_started_at DESC
        LIMIT )" << (endpoint_filter.empty() ? "500" : "100");
    
    return query.str();
}

crow::response ProjectConfigHandler::getProjectConfig(const crow::request& req) {
    try {
        if (!config_manager_) {
            return crow::response(500, "Internal server error: Configuration manager is not initialized");
        }
        auto config = config_manager_->getFlapiConfig();
        // Lets a client (the Claude Code hook, flapii) tell that edits to files
        // on disk cannot reach this server: a packed binary serves the copy
        // folded into it, and its config-service writes are refused.
        config["bundled"] = FileProviderFactory::GetBundleContents() != nullptr;
        return crow::response(200, config);
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

crow::response ProjectConfigHandler::updateProjectConfig(const crow::request& req) {
    try {
        auto json = crow::json::load(req.body);
        if (!json)
            return crow::response(400, "Invalid JSON");

        // TODO: Implement project config update logic
        return crow::response(501, "Not implemented");
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

crow::response ProjectConfigHandler::getEnvironmentVariables(const crow::request& req) {
    try {
        crow::json::wvalue response;
        response["variables"] = crow::json::wvalue::list();
        
        // The whitelist holds PATTERNS (regexes), not names. Report the variables
        // that actually exist in the environment and match one (#166): passing a
        // pattern such as `^DB_.*` to getenv() found nothing, and an exact-name
        // entry was the only thing that ever showed up.
        const auto& template_config = config_manager_->getTemplateConfig();
        size_t idx = 0;

        std::vector<std::string> names;
        for (char** e = ENV_BLOCK; e != nullptr && *e != nullptr; ++e) {
            const std::string entry(*e);
            const auto eq = entry.find('=');
            if (eq == std::string::npos || eq == 0) {
                continue;
            }
            const auto name = entry.substr(0, eq);
            if (template_config.isEnvironmentVariableAllowed(name)) {
                names.push_back(name);
            }
        }
        // A plain-identifier entry names one variable, so report it even when it
        // is unset ("is API_KEY configured?" -> available:false).
        static const std::regex plain_name(R"(^[A-Za-z_][A-Za-z0-9_]*$)");
        for (const auto& entry : template_config.environment_whitelist) {
            if (std::regex_match(entry, plain_name)) {
                names.push_back(entry);
            }
        }
        std::sort(names.begin(), names.end());
        names.erase(std::unique(names.begin(), names.end()), names.end());

        for (const auto& var_name : names) {
            crow::json::wvalue var;
            var["name"] = var_name;
            
            // Whether it is SET, not what it contains.
            //
            // This returned every whitelisted variable's value verbatim. It
            // was written for a route behind the config-service token, and
            // then an MCP tool was wired to it - at which point the same
            // values TemplateSecrets exists to scrub out of previews and
            // error messages were being handed over as the tool's whole
            // purpose. The token is required again now, but a caller asking
            // "is API_KEY configured?" never needed the key itself, and this
            // is the only answer that is safe to give over any transport.
            //
            // A value whose name does not look like a credential is still
            // shown, because that is the case operators actually debug with:
            // a region, a bucket, a path.
            const char* env_value = std::getenv(var_name.c_str());
            var["available"] = env_value != nullptr;
            if (env_value == nullptr) {
                var["value"] = "";
            } else if (isCredentialKey(var_name) ||
                       std::string(env_value).size() >= 24) {
                var["value"] = "<redacted>";
                var["redacted"] = true;
            } else {
                var["value"] = std::string(env_value);
            }
            
            response["variables"][idx++] = std::move(var);
        }
        
        return crow::response(200, response);
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

crow::response EndpointConfigHandler::listEndpoints(const crow::request& req) {
    try {
        auto endpoints = config_manager_->getEndpointsConfig();
        return crow::response(200, endpoints);
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

// ----------------------------------------------------------------------------
// endpoint_files: YAML persistence shared with the flapi_* MCP tools
// ----------------------------------------------------------------------------
namespace endpoint_files {

namespace {

std::string scalarOrEmpty(const YAML::Node& node, const char* key) {
    if (node && node.IsMap() && node[key] && node[key].IsScalar()) {
        return node[key].as<std::string>();
    }
    return "";
}

// The endpoint's identity, read from the document the way the loader reads it,
// so the file name and the duplicate check agree with what will be served.
EndpointConfig identityOf(const YAML::Node& doc) {
    EndpointConfig id;
    id.urlPath = scalarOrEmpty(doc, "url-path");
    id.method = scalarOrEmpty(doc, "method");
    if (id.method.empty()) {
        id.method = "GET";
    }
    if (doc["mcp-tool"]) {
        id.mcp_tool = EndpointConfig::MCPToolInfo{};
        id.mcp_tool->name = scalarOrEmpty(doc["mcp-tool"], "name");
    }
    if (doc["mcp-resource"]) {
        id.mcp_resource = EndpointConfig::MCPResourceInfo{};
        id.mcp_resource->name = scalarOrEmpty(doc["mcp-resource"], "name");
    }
    if (doc["mcp-prompt"]) {
        id.mcp_prompt = EndpointConfig::MCPPromptInfo{};
        id.mcp_prompt->name = scalarOrEmpty(doc["mcp-prompt"], "name");
    }
    return id;
}

std::string emit(const YAML::Node& doc) {
    YAML::Emitter out;
    out << doc;
    return std::string(out.c_str()) + "\n";
}

bool writeFile(const std::filesystem::path& file, const std::string& content, std::string& error) {
    std::error_code ec;
    if (file.has_parent_path()) {
        std::filesystem::create_directories(file.parent_path(), ec);
    }
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        error = "cannot write " + file.string();
        return false;
    }
    out << content;
    out.close();
    if (!out) {
        error = "cannot write " + file.string();
        return false;
    }
    return true;
}

// Maps merge key by key; anything else (a sequence such as `request`, a
// scalar) is replaced by the patch's value.
YAML::Node deepMerge(const YAML::Node& base, const YAML::Node& patch) {
    if (!base || !base.IsMap() || !patch.IsMap()) {
        return YAML::Clone(patch);
    }
    YAML::Node out = YAML::Clone(base);
    for (const auto& kv : patch) {
        const std::string key = kv.first.as<std::string>();
        if (out[key] && out[key].IsMap() && kv.second.IsMap()) {
            out[key] = deepMerge(out[key], kv.second);
        } else {
            out[key] = YAML::Clone(kv.second);
        }
    }
    return out;
}

// Parse the file with the loader's parser and validate it exactly as a reload
// would. On success `outcome.config` is what to publish.
bool parseAndValidate(ConfigManager& cm, const std::filesystem::path& file, Outcome& outcome) {
    EndpointConfigParser parser(cm.getYamlParser(), &cm);
    auto parsed = parser.parseFromFile(file);
    if (!parsed.success) {
        outcome.status = 400;
        outcome.error = parsed.error_message.empty() ? "Invalid endpoint configuration" : parsed.error_message;
        return false;
    }
    const auto validation = cm.validateEndpointConfig(parsed.config);
    if (!validation.valid) {
        std::string joined;
        for (const auto& e : validation.errors) {
            joined += (joined.empty() ? "" : "; ") + e;
        }
        outcome.status = 400;
        outcome.error = "Invalid endpoint: " + joined;
        return false;
    }
    outcome.config = parsed.config;
    return true;
}

// The template-source / cache template-file boundary (#189), checked on the
// document's own values before anything is written.
std::string templatePathProblem(const ConfigManager& cm, const YAML::Node& doc) {
    std::vector<std::string> sources = {scalarOrEmpty(doc, "template-source")};
    if (doc["cache"]) {
        sources.push_back(scalarOrEmpty(doc["cache"], "template-file"));
    }
    for (const auto& source : sources) {
        if (source.empty()) {
            continue;
        }
        const auto problem = cm.TemplatePathProblem(source);
        if (!problem.empty()) {
            return problem;
        }
    }
    return "";
}

// A file using flAPI's {{include ...}} directives cannot be loaded, merged and
// re-emitted as plain YAML without losing them.
bool usesIncludeDirectives(const std::string& content) {
    return content.find("{{include") != std::string::npos;
}

}  // namespace

std::string FileStemFor(const EndpointConfig& endpoint) {
    // url-path segments joined with '_' (/made-by-cli -> made-by-cli,
    // /a/b -> a_b, /customers/:id -> customers_id), else the MCP name. It used to
    // be the slug, whose escaping gave made~1by~1cli.yaml.
    const std::string source = !endpoint.urlPath.empty() ? endpoint.urlPath : endpoint.getName();
    std::string stem;
    bool pending_separator = false;
    for (const char c : source) {
        const bool keep = std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.';
        if (!keep) {
            pending_separator = !stem.empty();
            continue;
        }
        if (pending_separator) {
            stem += '_';
            pending_separator = false;
        }
        stem += c;
    }
    while (!stem.empty() && stem.front() == '.') {
        stem.erase(0, 1);   // never a hidden file, never ".."
    }
    return stem.empty() ? std::string("endpoint") : stem;
}

std::string FileDeclaring(const ConfigManager& cm, const std::string& url_path, const std::string& method) {
    std::error_code ec;
    const std::filesystem::path dir = cm.getTemplateConfig().path;
    if (!std::filesystem::is_directory(dir, ec)) {
        return "";
    }
    for (std::filesystem::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const auto ext = it->path().extension();
        if (!it->is_regular_file(ec) || (ext != ".yaml" && ext != ".yml")) {
            continue;
        }
        try {
            const YAML::Node doc = YAML::LoadFile(it->path().string());
            if (!doc || !doc.IsMap() || scalarOrEmpty(doc, "url-path") != url_path) {
                continue;
            }
            std::string declared = scalarOrEmpty(doc, "method");
            if (declared.empty()) {
                declared = "GET";
            }
            std::transform(declared.begin(), declared.end(), declared.begin(),
                           [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            if (declared == method) {
                return it->path().string();
            }
        } catch (const std::exception&) {
            // Not plain YAML (e.g. {{include}} directives); the loader judges it.
        }
    }
    return "";
}

std::string FileDeclaringMcpName(const ConfigManager& cm, const std::string& name) {
    std::error_code ec;
    const std::filesystem::path dir = cm.getTemplateConfig().path;
    if (name.empty() || !std::filesystem::is_directory(dir, ec)) {
        return "";
    }
    for (std::filesystem::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const auto ext = it->path().extension();
        if (!it->is_regular_file(ec) || (ext != ".yaml" && ext != ".yml")) {
            continue;
        }
        try {
            const YAML::Node doc = YAML::LoadFile(it->path().string());
            if (!doc || !doc.IsMap() || doc["url-path"]) {
                continue;
            }
            for (const char* kind : {"mcp-tool", "mcp-resource", "mcp-prompt"}) {
                if (scalarOrEmpty(doc[kind], "name") == name) {
                    return it->path().string();
                }
            }
        } catch (const std::exception&) {
            // Not plain YAML (e.g. {{include}} directives); the loader judges it.
        }
    }
    return "";
}

bool IsCanonicalDocument(const YAML::Node& doc) {
    return doc && doc.IsMap() &&
           (doc["url-path"] || doc["mcp-tool"] || doc["mcp-resource"] || doc["mcp-prompt"]);
}

Outcome CreateFromDocument(ConfigManager& cm, const YAML::Node& doc) {
    Outcome outcome;
    if (!IsCanonicalDocument(doc)) {
        outcome.status = 400;
        outcome.error = "An endpoint needs url-path, mcp-tool, mcp-resource or mcp-prompt";
        return outcome;
    }
    if (FileProviderFactory::GetBundleContents() != nullptr) {
        outcome.status = 409;
        outcome.error = "This server runs from a packed bundle; its configuration is read-only";
        return outcome;
    }
    if (const auto problem = templatePathProblem(cm, doc); !problem.empty()) {
        outcome.status = 400;
        outcome.error = problem;
        return outcome;
    }

    const EndpointConfig id = identityOf(doc);
    const auto existing = cm.getEndpoints();
    for (const auto& ep : *existing) {
        const bool same_rest = !id.urlPath.empty() && ep.urlPath == id.urlPath && ep.method == id.method;
        const bool same_name = id.urlPath.empty() && !id.getName().empty() && ep.getName() == id.getName();
        if (same_rest || same_name) {
            outcome.status = 409;
            outcome.error = "Endpoint already exists: " + id.getIdentifier() +
                            " (update it with PUT, or delete it first)";
            return outcome;
        }
    }

    // A YAML in the templates directory may declare the endpoint without being
    // loaded (written after startup, not reloaded): `flapii endpoints create -f
    // sqls/x.yaml` then wrote a second copy, and the next start had two files
    // for one url-path+method.
    if (!id.urlPath.empty()) {
        std::string method = id.method;
        std::transform(method.begin(), method.end(), method.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        if (const auto other = FileDeclaring(cm, id.urlPath, method); !other.empty()) {
            outcome.status = 409;
            outcome.error = "Endpoint already exists: " + id.method + " " + id.urlPath + " is declared in " + other +
                            " - load it with POST /api/v1/_config/endpoints/reload-file {\"file\": \"" + other +
                            "\"} (flapii endpoints reload --file " + other + ") instead of creating a copy";
            return outcome;
        }
    } else if (!id.getName().empty()) {
        // The MCP-only form of the same: an mcp-tool file written to sqls/ by hand.
        if (const auto other = FileDeclaringMcpName(cm, id.getName()); !other.empty()) {
            outcome.status = 409;
            outcome.error = "Endpoint already exists: MCP name '" + id.getName() + "' is declared in " + other +
                            " - load it with POST /api/v1/_config/endpoints/reload-file {\"file\": \"" + other +
                            "\"} (flapii endpoints reload --file " + other + ") instead of creating a copy";
            return outcome;
        }
    }

    // A readable file name - the same one flapi_create_endpoint gives it.
    const std::filesystem::path file =
        std::filesystem::path(cm.getTemplateConfig().path) / (FileStemFor(id) + ".yaml");
    if (std::filesystem::exists(file)) {
        outcome.status = 409;
        outcome.error = "Endpoint file already exists: " + file.string();
        return outcome;
    }

    if (!writeFile(file, emit(doc), outcome.error)) {
        outcome.status = 500;
        return outcome;
    }
    auto cleanup = [&]() {
        std::error_code ec;
        std::filesystem::remove(file, ec);
    };
    try {
        if (!parseAndValidate(cm, file, outcome)) {
            cleanup();
            return outcome;
        }
        // url-path+method AND every MCP name against the loaded table: a
        // REST+tool file reusing a loaded tool name loaded silently before.
        if (const auto duplicate = cm.DuplicateEndpointProblem(outcome.config); !duplicate.empty()) {
            cleanup();
            outcome.status = 409;
            outcome.error = duplicate;
            return outcome;
        }
        cm.addEndpoint(outcome.config);
    } catch (const std::exception& e) {
        cleanup();
        outcome.status = 500;
        outcome.error = e.what();
        return outcome;
    }
    outcome.ok = true;
    outcome.status = 201;
    outcome.file = file.string();
    CROW_LOG_INFO << "Created endpoint " << outcome.config.getIdentifier() << " in " << file;
    return outcome;
}

Outcome UpdateFromPatch(ConfigManager& cm, const EndpointConfig& existing, const YAML::Node& patch) {
    Outcome outcome;
    if (FileProviderFactory::GetBundleContents() != nullptr) {
        outcome.status = 409;
        outcome.error = "This server runs from a packed bundle; its configuration is read-only";
        return outcome;
    }

    std::filesystem::path file = existing.config_file_path;
    std::string previous;
    bool had_file = false;
    YAML::Node base;
    if (!file.empty() && std::filesystem::exists(file)) {
        std::ifstream in(file, std::ios::binary);
        std::stringstream buffer;
        buffer << in.rdbuf();
        previous = buffer.str();
        had_file = true;
        if (usesIncludeDirectives(previous)) {
            outcome.status = 409;
            outcome.error = "The endpoint's YAML (" + file.string() +
                            ") uses {{include}} directives, which an API update would flatten; "
                            "edit the file and reload the endpoint instead";
            return outcome;
        }
        base = YAML::Load(previous);
    } else {
        // Defined in memory only (created before writes persisted): start the
        // file from its serialised form.
        file = std::filesystem::path(cm.getTemplateConfig().path) / (FileStemFor(existing) + ".yaml");
        base = YAML::Load(cm.serializeEndpointConfigToYaml(existing));
    }

    const YAML::Node merged = deepMerge(base, patch);
    if (const auto problem = templatePathProblem(cm, merged); !problem.empty()) {
        outcome.status = 400;
        outcome.error = problem;
        return outcome;
    }
    // The update must still name the same endpoint: renaming it is a delete
    // plus a create, and replaceEndpoint would otherwise silently miss.
    if (!identityOf(merged).isSameEndpoint(existing) ||
        (existing.isRESTEndpoint() && identityOf(merged).method != existing.method)) {
        outcome.status = 400;
        outcome.error = "The update changes the endpoint's url-path, method or name; delete and re-create it instead";
        return outcome;
    }

    auto restore = [&]() {
        std::error_code ec;
        if (had_file) {
            std::string ignored;
            writeFile(file, previous, ignored);
        } else {
            std::filesystem::remove(file, ec);
        }
    };
    if (!writeFile(file, emit(merged), outcome.error)) {
        outcome.status = 500;
        restore();
        return outcome;
    }
    try {
        if (!parseAndValidate(cm, file, outcome)) {
            restore();
            return outcome;
        }
        if (!cm.replaceEndpoint(outcome.config)) {
            restore();
            outcome.status = 404;
            outcome.error = "Endpoint not found";
            return outcome;
        }
    } catch (const std::exception& e) {
        restore();
        outcome.status = 500;
        outcome.error = e.what();
        return outcome;
    }
    outcome.ok = true;
    outcome.status = 200;
    outcome.file = file.string();
    CROW_LOG_INFO << "Updated endpoint " << outcome.config.getIdentifier() << " in " << file;
    return outcome;
}

std::string RemoveEndpointFile(const ConfigManager& cm, const std::string& deleted_file) {
    if (deleted_file.empty() || FileProviderFactory::GetBundleContents() != nullptr) {
        return "";
    }
    std::error_code ec;
    const auto template_dir = std::filesystem::weakly_canonical(cm.getTemplateConfig().path, ec);
    const auto file = std::filesystem::weakly_canonical(deleted_file, ec);
    const auto rel = file.lexically_relative(template_dir);
    if (rel.empty() || *rel.begin() == "..") {
        return "";
    }
    std::filesystem::remove(file, ec);
    if (ec) {
        return "removed from memory but could not delete " + file.string() + ": " + ec.message();
    }
    return "";
}

std::string ReloadFailureReason(const ConfigManager& cm, const EndpointConfig& endpoint) {
    if (endpoint.config_file_path.empty()) {
        return "The endpoint was not loaded from a YAML file, so there is nothing to reload it from";
    }
    if (!std::filesystem::exists(endpoint.config_file_path)) {
        return "YAML file not found: " + endpoint.config_file_path;
    }
    const auto validation = cm.validateEndpointConfigFile(endpoint.config_file_path);
    std::string joined;
    for (const auto& e : validation.errors) {
        joined += (joined.empty() ? "" : "; ") + e;
    }
    if (!joined.empty()) {
        return endpoint.config_file_path + ": " + joined;
    }
    try {
        auto& mutable_cm = const_cast<ConfigManager&>(cm);   // the parser API is non-const; nothing is changed
        EndpointConfigParser parser(mutable_cm.getYamlParser(), &mutable_cm);
        const auto parsed = parser.parseFromFile(endpoint.config_file_path);
        if (parsed.success) {
            if (const auto duplicate = cm.DuplicateEndpointProblem(parsed.config); !duplicate.empty()) {
                return duplicate;
            }
        }
    } catch (const std::exception&) {
        // fall through to the generic reason
    }
    return "Failed to reload endpoint configuration from " + endpoint.config_file_path;
}

}  // namespace endpoint_files

namespace {

// A config-service error as JSON, so the CLI can print the reason rather than
// the bare status text.
crow::response jsonError(int status, const std::string& message) {
    crow::json::wvalue body;
    body["error"] = message;
    crow::response res(status, body);
    res.set_header("Content-Type", "application/json");
    return res;
}

crow::response persistedResponse(int status, const endpoint_files::Outcome& outcome) {
    crow::json::wvalue body;
    body["status"] = status == 201 ? "created" : "updated";
    body["endpoint"] = outcome.config.getIdentifier();
    body["slug"] = outcome.config.getSlug();
    if (!outcome.config.urlPath.empty()) {
        body["url-path"] = outcome.config.urlPath;
    }
    body["file"] = outcome.file;
    crow::response res(status, body);
    res.set_header("Content-Type", "application/json");
    return res;
}

// The request body as an endpoint document. JSON is YAML, so a JSON body and
// the YAML file `flapii endpoints create -f x.yaml` read from parse alike.
std::optional<YAML::Node> bodyAsDocument(const std::string& body) {
    try {
        YAML::Node doc = YAML::Load(body);
        if (doc && doc.IsMap()) {
            return doc;
        }
    } catch (const std::exception&) {
        // not YAML/JSON
    }
    return std::nullopt;
}

}  // namespace

crow::response EndpointConfigHandler::createEndpoint(const crow::request& req) {
    try {
        auto doc = bodyAsDocument(req.body);
        if (!doc) {
            return jsonError(400, "Invalid JSON");
        }

        // A body with the legacy camelCase keys (urlPath, templateSource) is
        // what only the JSON deserializer understands; turn it into the YAML
        // document it describes. A canonical body is used as sent, so nothing
        // the JSON deserializer does not know - mcp-tool, request validators,
        // rate-limit - is dropped on the way (it used to be: `flapii endpoints
        // create -f x.yaml` produced a REST-only endpoint).
        if (!endpoint_files::IsCanonicalDocument(*doc)) {
            auto json = crow::json::load(req.body);
            if (!json) {
                return jsonError(400, "Invalid JSON");
            }
            doc = YAML::Load(config_manager_->serializeEndpointConfigToYaml(jsonToEndpointConfig(json)));
        }

        // Persisted like flapi_create_endpoint: the YAML is written to the
        // templates directory, so the endpoint survives a reload and a restart.
        // This used to change memory only.
        const auto outcome = endpoint_files::CreateFromDocument(*config_manager_, *doc);
        if (!outcome.ok) {
            return jsonError(outcome.status, outcome.error);
        }
        return persistedResponse(201, outcome);
    } catch (const std::exception& e) {
        return jsonError(500, std::string("Internal server error: ") + e.what());
    }
}

// Helper method to find endpoint by slug (centralized slug logic).
//
// Returns an EndpointRef, not a raw pointer. The snapshot is copy-on-write: an
// addEndpoint / removeEndpointByPath / replaceEndpoint on another thread
// releases the vector, so a pointer into it outlives its owner the moment the
// local pin goes out of scope. This function used to return exactly that -
// `const auto endpoints = ...` pinned the snapshot for the duration of the LOOP
// and handed the caller a pointer into a vector nobody was holding any more.
//
// ConfigManager::EndpointRef exists for this and carries the pin with the
// pointer; getEndpointForPath already returns one. Bind it with `const auto`,
// never `const auto*` - the missing pointer conversion is deliberate, and the
// rvalue `.get()` overload is deleted, so the mistake is a compile error.
ConfigManager::EndpointRef findEndpointBySlug(std::shared_ptr<ConfigManager> config_manager, const std::string& slug) {
    auto endpoints = config_manager->getEndpoints();   // pinned snapshot
    for (const auto& endpoint : *endpoints) {
        if (endpoint.getSlug() == slug) {
            return ConfigManager::EndpointRef(std::move(endpoints), &endpoint);
        }
    }
    return {};
}

// New slug-based methods (centralized, works for both REST and MCP)
crow::response EndpointConfigHandler::getEndpointConfigBySlug(const crow::request& req, const std::string& slug) {
    try {
        const auto endpoint = findEndpointBySlug(config_manager_, slug);
        if (!endpoint) {
            return crow::response(404, "Endpoint not found");
        }
        return crow::response(200, endpointConfigToJson(*endpoint));
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

crow::response EndpointConfigHandler::updateExisting(const crow::request& req, const EndpointConfig& existing) {
    auto json = crow::json::load(req.body);
    auto doc = bodyAsDocument(req.body);
    if (!json || !doc) {
        return jsonError(400, "Invalid JSON");
    }

    // A partial body in the YAML (hyphenated) spelling - `{"template-source":
    // "x.sql"}`, `{"method": "POST"}` - is a patch like a full document: the
    // target supplies url-path. It used to fall into the legacy camelCase path
    // below, which requires url-path and template-source, and answered 500
    // "Missing required field in endpoint config".
    const bool legacy_keys = [&]() {
        for (const auto& entry : *doc) {
            const std::string key = entry.first.as<std::string>("");
            for (const char c : key) {
                if (c == '_' || (c >= 'A' && c <= 'Z')) {
                    return true;
                }
            }
        }
        return false;
    }();

    if (endpoint_files::IsCanonicalDocument(*doc) || !legacy_keys) {
        // Deep-merged onto the endpoint's YAML and re-parsed by the loader, so
        // the file, memory and the next restart agree. Updates used to change
        // memory only and were lost on reload.
        const auto outcome = endpoint_files::UpdateFromPatch(*config_manager_, existing, *doc);
        if (!outcome.ok) {
            return jsonError(outcome.status, outcome.error);
        }
        return persistedResponse(200, outcome);
    }

    // Legacy camelCase body: only the JSON deserializer reads it, and its
    // lossy serialised form must not overwrite a hand-written file (it would
    // strip validators and annotations). Applied in memory, as before, and
    // said so.
    // The target supplies what a partial camelCase body leaves out.
    crow::json::wvalue completed(json);
    if (!json.has("urlPath") && !json.has("url_path") && !json.has("url-path")) {
        completed["urlPath"] = existing.urlPath;
    }
    if (!json.has("templateSource") && !json.has("template_source") && !json.has("template-source")) {
        completed["templateSource"] = existing.templateSource;
    }
    if (!json.has("method") && !json.has("Method")) {
        completed["method"] = existing.method;
    }
    const auto completed_json = crow::json::load(completed.dump());
    EndpointConfig updated_config;
    try {
        updated_config = jsonToEndpointConfig(completed_json, /*merge_existing=*/true);
    } catch (const std::exception& e) {
        // A body the deserializer cannot read is the caller's error, not ours.
        return jsonError(400, std::string("Invalid endpoint configuration: ") + e.what());
    }
    if (!updated_config.isSameEndpoint(existing)) {
        return jsonError(400, "URL path in config does not match endpoint path");
    }
    for (const auto& source : {updated_config.templateSource,
                               updated_config.cache.template_file.value_or(std::string())}) {
        const auto problem = config_manager_->TemplatePathProblem(source);
        if (!problem.empty()) {
            return jsonError(400, problem);
        }
    }
    if (!config_manager_->replaceEndpoint(updated_config)) {
        return jsonError(404, "Endpoint not found");
    }
    crow::json::wvalue body;
    body["status"] = "updated";
    body["persisted"] = false;
    body["warning"] = "camelCase body applied in memory only; send the YAML (hyphenated) keys to persist";
    return crow::response(200, body);
}

crow::response EndpointConfigHandler::updateEndpointConfigBySlug(const crow::request& req, const std::string& slug) {
    try {
        const auto endpoint = findEndpointBySlug(config_manager_, slug);
        if (!endpoint) {
            return jsonError(404, "Endpoint not found");
        }
        return updateExisting(req, *endpoint);
    } catch (const std::exception& e) {
        return jsonError(500, std::string("Internal server error: ") + e.what());
    }
}

crow::response EndpointConfigHandler::removeExisting(const EndpointConfig& existing) {
    const std::string file = existing.config_file_path;
    // Exactly this endpoint: by path it removed GET /orders with POST /orders.
    if (!config_manager_->removeEndpoint(existing)) {
        return jsonError(404, "Endpoint not found");
    }
    // Deleting persists, like flapi_delete_endpoint: the YAML goes too, so the
    // endpoint does not come back on the next reload or restart.
    const auto warning = endpoint_files::RemoveEndpointFile(*config_manager_, file);
    crow::json::wvalue body;
    body["status"] = "deleted";
    body["endpoint"] = existing.getIdentifier();
    if (!warning.empty()) {
        CROW_LOG_WARNING << "delete " << existing.getIdentifier() << ": " << warning;
        body["warning"] = warning;
    }
    return crow::response(200, body);
}

crow::response EndpointConfigHandler::deleteEndpointBySlug(const crow::request& req, const std::string& slug) {
    try {
        const auto endpoint = findEndpointBySlug(config_manager_, slug);
        if (!endpoint) {
            return jsonError(404, "Endpoint not found");
        }
        return removeExisting(*endpoint);
    } catch (const std::exception& e) {
        return jsonError(500, std::string("Internal server error: ") + e.what());
    }
}

// Legacy path-based method (kept for backward compatibility)
crow::response EndpointConfigHandler::getEndpointConfig(const crow::request& req, const std::string& path) {
    try {
        const auto endpoint = config_manager_->getEndpointForKey(path);
        if (!endpoint) {
            return crow::response(404, "Endpoint not found");
        }

        return crow::response(200, endpointConfigToJson(*endpoint));
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

// Helper methods for converting between JSON and EndpointConfig
crow::json::wvalue EndpointConfigHandler::endpointConfigToJson(const EndpointConfig& config) {
    return config_manager_->serializeEndpointConfig(config, EndpointJsonStyle::HyphenCase);
}

EndpointConfig EndpointConfigHandler::jsonToEndpointConfig(const crow::json::rvalue& json, bool merge_existing) {
    return config_manager_->deserializeEndpointConfig(json, merge_existing);
}

crow::response EndpointConfigHandler::updateEndpointConfig(const crow::request& req, const std::string& path) {
    try {
        const auto endpoint = config_manager_->getEndpointForKey(path);
        if (!endpoint) {
            return jsonError(404, "Endpoint not found");
        }
        return updateExisting(req, *endpoint);
    } catch (const std::exception& e) {
        return jsonError(500, std::string("Internal server error: ") + e.what());
    }
}

crow::response EndpointConfigHandler::deleteEndpoint(const crow::request& req, const std::string& path) {
    try {
        const auto endpoint = config_manager_->getEndpointForKey(path);
        if (!endpoint) {
            return jsonError(404, "Endpoint not found");
        }
        return removeExisting(*endpoint);
    } catch (const std::exception& e) {
        return jsonError(500, std::string("Internal server error: ") + e.what());
    }
}

crow::response EndpointConfigHandler::validateEndpointConfig(const crow::request& req, const std::string& path) {
    auto invalid = [](const std::string& message) {
        crow::json::wvalue response;
        response["valid"] = false;
        response["errors"] = crow::json::wvalue::list();
        response["errors"][0] = message;
        return crow::response(400, response);
    };
    try {
        std::string yaml_content = req.body;
        if (yaml_content.find_first_not_of(" \t\r\n") == std::string::npos) {
            // JSON like every other answer here, so a client reading `errors`
            // shows the reason instead of `valid: false` with nothing under it.
            return invalid("No YAML given: send the endpoint YAML as the request body");
        }

        // Validated with the LOADER's parser, from a file in the directory the
        // YAML lives in - the endpoint's own directory when it already exists,
        // the templates directory otherwise - so template-source and
        // {{include}} resolve exactly as they will when it is loaded. This used
        // deserializeEndpointConfigFromYaml, which ignored request fields and
        // validators and resolved template-source against template.path, so it
        // passed files the server then failed to load, and failed files that
        // loaded fine.
        auto doc = bodyAsDocument(yaml_content);
        ConfigManager::ValidationResult validation{true, {}, {}};
        std::filesystem::path dir = config_manager_->getTemplateConfig().path;
        if (doc) {
            const auto existing = config_manager_->getEndpointForKey(path);
            if (existing && !existing->config_file_path.empty()) {
                dir = std::filesystem::path(existing->config_file_path).parent_path();
            }
        }

        if (FileProviderFactory::GetBundleContents() != nullptr || !std::filesystem::is_directory(dir)) {
            // Nowhere to put a scratch file (packed bundle): best effort.
            validation = config_manager_->validateEndpointConfigFromYaml(yaml_content);
        } else {
            static std::atomic<uint64_t> counter{0};
            const auto scratch = dir / (".flapi-validate-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                std::to_string(counter.fetch_add(1)) + ".yaml");
            {
                std::ofstream out(scratch, std::ios::binary | std::ios::trunc);
                if (!out.is_open()) {
                    return invalid("Validation error: cannot write a scratch file in " + dir.string());
                }
                out << yaml_content;
            }
            validation = config_manager_->validateEndpointConfigFile(scratch);
            std::error_code ec;
            std::filesystem::remove(scratch, ec);
            // The scratch file's name must not leak into the messages.
            for (auto* list : {&validation.errors, &validation.warnings}) {
                for (auto& message : *list) {
                    const auto at = message.find(scratch.string());
                    if (at != std::string::npos) {
                        message.replace(at, scratch.string().size(), "<request body>");
                    }
                }
            }
        }

        crow::json::wvalue response;
        response["valid"] = validation.valid;

        if (!validation.errors.empty()) {
            response["errors"] = crow::json::wvalue::list();
            for (size_t i = 0; i < validation.errors.size(); ++i) {
                response["errors"][i] = validation.errors[i];
            }
        }

        if (!validation.warnings.empty()) {
            response["warnings"] = crow::json::wvalue::list();
            for (size_t i = 0; i < validation.warnings.size(); ++i) {
                response["warnings"][i] = validation.warnings[i];
            }
        }

        int32_t status_code = validation.valid ? 200 : 400;
        return crow::response(status_code, response);
    } catch (const std::exception& e) {
        return invalid(std::string("Validation error: ") + e.what());
    }
}

crow::response EndpointConfigHandler::reloadEndpointConfig(const crow::request& req, const std::string& path) {
    try {
        // Known first, so "no such endpoint" (404) and "it exists but its file
        // is broken" are told apart. Both answered 404 "Failed to reload", so a
        // typo in the YAML looked like a wrong slug.
        // By key: the route resolved slug + ?method= to exactly one endpoint.
        const auto target = config_manager_->getEndpointForKey(path);
        crow::json::wvalue response;
        // `error` like create/update/delete answer, so a client reads every
        // config-service failure the same way; `message`/`reason` stay.
        if (!target) {
            response["success"] = false;
            response["error"] = "Endpoint not found: " + path;
            response["message"] = "Endpoint not found: " + path;
            return crow::response(404, response);
        }

        const std::string file = target->config_file_path;
        const bool success = config_manager_->reloadEndpointConfig(path);
        response["success"] = success;
        if (success) {
            if (!file.empty() && !std::filesystem::exists(file)) {
                // Its YAML was deleted: reloading unloaded it (and anything
                // else that file declared).
                response["status"] = "removed";
                response["message"] = "Endpoint file " + file + " no longer exists; the endpoint was unloaded";
                response["removed"] = crow::json::wvalue::list({ConfigManager::EndpointKey(*target)});
                return crow::response(200, response);
            }
            response["message"] = "Endpoint configuration reloaded successfully";
            return crow::response(200, response);
        }
        response["message"] = "Failed to reload endpoint configuration";
        response["error"] = "Failed to reload endpoint configuration";
        response["reason"] = endpoint_files::ReloadFailureReason(*config_manager_, *target);
        return crow::response(400, response);
    } catch (const std::exception& e) {
        crow::json::wvalue response;
        response["success"] = false;
        response["error"] = std::string("Reload error: ") + e.what();
        response["message"] = std::string("Reload error: ") + e.what();
        return crow::response(500, response);
    }
}

crow::response EndpointConfigHandler::reloadEndpointFile(const crow::request& req) {
    crow::json::wvalue response;
    const auto fail = [&response](int status, const std::string& message) {
        response["success"] = false;
        response["error"] = message;
        crow::response res(status, response);
        res.set_header("Content-Type", "application/json");
        return res;
    };
    try {
        auto json = crow::json::load(req.body);
        if (!json || json.t() != crow::json::type::Object || !json.has("file") ||
            json["file"].t() != crow::json::type::String) {
            return fail(400, "Invalid JSON: expected {\"file\": \"<path to the endpoint YAML>\"}");
        }
        if (FileProviderFactory::GetBundleContents() != nullptr) {
            return fail(409, "This server runs from a packed bundle; its configuration is read-only");
        }
        std::filesystem::path file = std::string(json["file"].s());
        if (file.is_relative()) {
            file = std::filesystem::path(config_manager_->getBasePath()) / file;
        }
        // Only files the loader itself would read: the templates directory.
        // Anything else is not an endpoint at the next restart either.
        std::error_code ec;
        const auto template_dir = std::filesystem::weakly_canonical(config_manager_->getTemplateConfig().path, ec);
        const auto canonical = std::filesystem::weakly_canonical(file, ec);
        const auto rel = canonical.lexically_relative(template_dir);
        if (rel.empty() || *rel.begin() == "..") {
            return fail(400, file.string() + " is outside template.path (" + template_dir.string() +
                                 "); only endpoint YAMLs there are loaded");
        }
        const auto ext = canonical.extension();
        if (ext != ".yaml" && ext != ".yml") {
            return fail(400, file.string() + " is not a YAML file");
        }

        const auto result = config_manager_->reloadEndpointFile(canonical);
        if (!result.ok) {
            return fail(result.status, result.error);
        }
        response["success"] = true;
        response["file"] = canonical.string();
        if (!result.removed.empty()) {
            // The file was deleted: what it declared is unloaded.
            response["status"] = "removed";
            response["message"] = "Endpoint file no longer exists; its endpoints were unloaded";
            crow::json::wvalue removed = crow::json::wvalue::list();
            for (std::size_t i = 0; i < result.removed.size(); ++i) {
                removed[i] = result.removed[i];
            }
            response["removed"] = std::move(removed);
            response["endpoints"] = crow::json::wvalue::list();
            crow::response res(200, response);
            res.set_header("Content-Type", "application/json");
            return res;
        }
        response["status"] = result.loaded_new ? "loaded" : "reloaded";
        response["message"] = result.loaded_new ? "Endpoint configuration loaded" : "Endpoint configuration reloaded";
        crow::json::wvalue keys = crow::json::wvalue::list();
        for (std::size_t i = 0; i < result.endpoints.size(); ++i) {
            keys[i] = result.endpoints[i];
        }
        response["endpoints"] = std::move(keys);
        if (!result.warnings.empty()) {
            crow::json::wvalue warnings = crow::json::wvalue::list();
            for (std::size_t i = 0; i < result.warnings.size(); ++i) {
                warnings[i] = result.warnings[i];
            }
            response["warnings"] = std::move(warnings);
        }
        crow::response res(result.status, response);
        res.set_header("Content-Type", "application/json");
        return res;
    } catch (const std::exception& e) {
        return fail(500, std::string("Reload error: ") + e.what());
    }
}

crow::response EndpointConfigHandler::getEndpointParameters(const crow::request& req, const std::string& path) {
    try {
        const auto endpoint = config_manager_->getEndpointForKey(path);
        if (!endpoint) {
            return crow::response(404, "Endpoint not found");
        }

        crow::json::wvalue response;
        response["parameters"] = crow::json::wvalue::list();
        
        // Extract request field definitions
        size_t idx = 0;
        for (const auto& field : endpoint->request_fields) {
            crow::json::wvalue param;
            param["name"] = field.fieldName;
            param["in"] = field.fieldIn;
            param["description"] = field.description;
            param["required"] = field.required;
            
            if (!field.defaultValue.empty()) {
                param["default"] = field.defaultValue;
            }
            
            // Add validator info if present
            if (!field.validators.empty()) {
                crow::json::wvalue validators_list = crow::json::wvalue::list();
                for (const auto& validator : field.validators) {
                    crow::json::wvalue v;
                    v["type"] = validator.type;
                    
                    if (validator.type == "int") {
                        v["min"] = validator.min;
                        v["max"] = validator.max;
                    } else if (validator.type == "string" && !validator.regex.empty()) {
                        v["regex"] = validator.regex;
                    } else if (validator.type == "enum") {
                        crow::json::wvalue allowed = crow::json::wvalue::list();
                        for (const auto& val : validator.allowedValues) {
                            allowed[allowed.size()] = val;
                        }
                        v["allowedValues"] = std::move(allowed);
                    }
                    
                    validators_list[validators_list.size()] = std::move(v);
                }
                param["validators"] = std::move(validators_list);
            }
            
            response["parameters"][idx++] = std::move(param);
        }
        
        // Add endpoint metadata
        response["endpoint"] = endpoint->getName();
        response["method"] = endpoint->method;
        
        return crow::response(200, response);
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

crow::response EndpointConfigHandler::findEndpointsByTemplate(const crow::request& req, const std::string& template_path) {
    try {
        crow::json::wvalue response;
        response["endpoints"] = crow::json::wvalue::list();
        
        // Normalize the template path for comparison
        auto normalized_template = std::filesystem::path(template_path).lexically_normal();
        
        // Search through all endpoints
        const auto endpoints = config_manager_->getEndpoints();   // pinned snapshot
        size_t idx = 0;
        
        // An endpoint created through the API keeps its template paths relative
        // to the templates directory; only file-loaded ones are absolute.
        const auto normalize = [this](const std::string& source) {
            std::filesystem::path p(source);
            if (p.is_relative()) {
                p = std::filesystem::path(config_manager_->getTemplateConfig().path) / p;
            }
            return p.lexically_normal();
        };

        for (const auto& endpoint : *endpoints) {
            // The template-source, and the cache's template-file: editing a
            // cache populate SQL used to find no endpoint, so nothing was
            // reloaded or re-rendered for it.
            std::vector<std::string> roles;
            if (!endpoint.templateSource.empty() && normalize(endpoint.templateSource) == normalized_template) {
                roles.push_back("template-source");
            }
            if (endpoint.cache.template_file && !endpoint.cache.template_file->empty() &&
                normalize(*endpoint.cache.template_file) == normalized_template) {
                roles.push_back("cache.template-file");
            }

            if (!roles.empty()) {
                crow::json::wvalue ep;
                // `role` is the first match; `roles` all of them (one file can
                // be both, though that is unusual).
                ep["role"] = roles.front();
                crow::json::wvalue role_list = crow::json::wvalue::list();
                for (std::size_t r = 0; r < roles.size(); ++r) {
                    role_list[r] = roles[r];
                }
                ep["roles"] = std::move(role_list);
                ep["slug"] = endpoint.getSlug();
                ep["url_path"] = endpoint.urlPath;
                ep["method"] = endpoint.method;
                ep["config_file_path"] = endpoint.config_file_path;
                ep["template_source"] = endpoint.templateSource;
                
                // Add endpoint type info
                if (endpoint.isRESTEndpoint()) {
                    ep["type"] = "REST";
                } else if (endpoint.isMCPTool()) {
                    ep["type"] = "MCP_Tool";
                    ep["mcp_name"] = endpoint.mcp_tool->name;
                } else if (endpoint.isMCPResource()) {
                    ep["type"] = "MCP_Resource";
                    ep["mcp_name"] = endpoint.mcp_resource->name;
                } else if (endpoint.isMCPPrompt()) {
                    ep["type"] = "MCP_Prompt";
                    ep["mcp_name"] = endpoint.mcp_prompt->name;
                }
                
                // TODO: Include full configuration to avoid additional API calls
                // Currently disabled due to Crow JSON serialization issues in tests
                // ep["full_config"] = config_manager_->serializeEndpointConfig(endpoint, EndpointJsonStyle::HyphenCase);
                
                response["endpoints"][idx++] = std::move(ep);
            }
        }
        
        response["count"] = static_cast<int>(idx);
        response["template_path"] = template_path;
        
        return crow::response(200, response);
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

crow::response TemplateHandler::getEndpointTemplate(const crow::request& req, const std::string& path) {
    try {
        // Find the endpoint
        auto endpoint = config_manager_->getEndpointForKey(path);
        if (!endpoint) {
            return crow::response(404, "Endpoint not found");
        }

        // Read the template file
        auto template_path = resolveTemplatePath(endpoint->templateSource);
        std::ifstream file(template_path);
        if (!file.is_open()) {
            return crow::response(500, "Could not open template file: " + template_path.string());
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        
        crow::json::wvalue response;
        response["template"] = buffer.str();
        return crow::response(200, response);
    } catch (const std::invalid_argument& e) {
        return crow::response(400, e.what());
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

crow::response TemplateHandler::updateEndpointTemplate(const crow::request& req, const std::string& path) {
    try {
        auto json = crow::json::load(req.body);
        if (!json || !json.has("template")) {
            return crow::response(400, "Invalid JSON: missing 'template' field");
        }

        // Find the endpoint
        auto endpoint = config_manager_->getEndpointForKey(path);
        if (!endpoint) {
            return crow::response(404, "Endpoint not found");
        }

        // Refuse a template crow would crash or hang on BEFORE it replaces a
        // working file: every request re-reads the template from disk.
        if (const auto syntax = CheckMustacheSyntax(json["template"].s())) {
            return jsonError(400, "Invalid template " + FormatMustacheSyntaxError(*syntax, endpoint->templateSource));
        }

        // Write the template to file
        auto template_path = resolveTemplatePath(endpoint->templateSource);
        std::error_code mkdir_ec;
        std::filesystem::create_directories(template_path.parent_path(), mkdir_ec);
        std::ofstream file(template_path);
        if (!file.is_open()) {
            return crow::response(500, "Could not open template file for writing: " + template_path.string());
        }

        file << json["template"].s();
        if (!file.good()) {
            return crow::response(500, "Error writing template to file");
        }

        return crow::response(200);
    } catch (const std::invalid_argument& e) {
        return crow::response(400, e.what());
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

crow::response TemplateHandler::expandTemplate(const crow::request& req, const std::string& path) {
    try {
        auto json = crow::json::load(req.body);
        if (!json || !json.has("parameters")) {
            return crow::response(400, "Invalid JSON: missing 'parameters' field");
        }

        // Check query parameters
        auto url_params = req.url_params;
        bool include_variables = url_params.get("include_variables") != nullptr;
        bool validate_only = url_params.get("validate_only") != nullptr;

        // Find the endpoint
        auto endpoint = config_manager_->getEndpointForKey(path);
        if (!endpoint) {
            return crow::response(404, "Endpoint not found");
        }

        // ?target=cache: the cache POPULATE template, rendered exactly as the
        // next refresh would render it - live watermark, snapshot ids, cursor
        // and key context - and executed nowhere. Developing an incremental
        // cache query is otherwise blind: the watermark only exists inside a
        // refresh.
        const char* target = url_params.get("target");
        if (target != nullptr && std::string(target) == "cache") {
            if (!endpoint->cache.enabled || endpoint->cache.table.empty()) {
                return crow::response(400, "Cache not enabled for this endpoint");
            }
            auto db_manager = DatabaseManager::getInstance();
            auto cache_manager = db_manager ? db_manager->getCacheManager() : nullptr;
            if (!cache_manager || !config_manager_->getDuckLakeConfig().enabled) {
                return crow::response(400, "DuckLake not enabled");
            }
            std::map<std::string, std::string> cache_params;
            std::string rendered;
            try {
                rendered = cache_manager->renderRefreshTemplate(config_manager_, *endpoint, cache_params);
            } catch (const std::invalid_argument& e) {
                // The Mustache checker's "Invalid template <file>:<line>:<col>: ...".
                // A broken template is the caller's input, not a server fault.
                if (validate_only) {
                    crow::json::wvalue response;
                    crow::json::wvalue errors = crow::json::wvalue::list();
                    crow::json::wvalue error;
                    error["type"] = "template_syntax";
                    error["statement"] = 0;
                    error["message"] = e.what();
                    errors[0] = std::move(error);
                    response["target"] = "cache";
                    response["valid"] = false;
                    response["errors"] = std::move(errors);
                    response["warnings"] = crow::json::wvalue::list();
                    return crow::response(200, response);
                }
                return jsonError(400, e.what());
            }
            const auto secrets = collectTemplateSecrets(config_manager_.get(), *endpoint, cache_params);
            crow::json::wvalue response;
            response["target"] = "cache";
            response["mode"] = cache_params.count("cacheMode") ? cache_params.at("cacheMode") : std::string();
            response["expanded"] = secrets.withhold()
                                       ? std::string("<expansion withheld: a value this template "
                                                     "interpolates is a credential too short to "
                                                     "redact reliably>")
                                       : secrets.scrub(rendered);
            if (validate_only) {
                // Checked as the next refresh would run it - with this watermark,
                // the bare-SELECT rule, and the materialisation a cache without a
                // template-file gets - but never run: parsed per statement and
                // prepared (bound) in a transaction that is rolled back. A
                // `MERGE INTOO`, an unknown column or a bare SELECT used to pass
                // here and fail at the next scheduled refresh.
                const PopulateTarget populate = *cacheTableOf(*config_manager_, *endpoint);
                std::string script = rendered;
                if (!endpoint->cache.template_file) {
                    if (auto materialised = DatabaseManager::MaterialiseSelect(rendered, populate)) {
                        script = std::move(*materialised);
                    }
                }
                // Serialised with refreshes: validation executes the template's
                // READ_ONLY ATTACH/DETACH, and attach names are instance-wide -
                // run beside a refresh attaching the same alias, it would hit
                // the collision that invalidates the database.
                std::unique_lock<std::mutex> catalog_lock;
                if (auto cache_manager = db_manager->getCacheManager()) {
                    catalog_lock = cache_manager->lockCatalogWrites();
                }
                const auto validation = db_manager->validateScript(script, &populate, &populate);
                if (catalog_lock.owns_lock()) {
                    catalog_lock.unlock();
                }
                crow::json::wvalue errors = crow::json::wvalue::list();
                crow::json::wvalue warnings = crow::json::wvalue::list();
                appendScriptIssues(validation, secrets, errors, warnings);
                if (const auto it = cache_params.find(CacheManager::kWatermarkDroppedReasonKey);
                    it != cache_params.end()) {
                    crow::json::wvalue warning;
                    warning["type"] = "watermark_dropped";
                    warning["statement"] = 0;
                    warning["message"] = secrets.scrub(it->second);
                    warnings[warnings.size()] = std::move(warning);
                }
                response["valid"] = validation.errors.empty();
                response["statements"] = static_cast<uint64_t>(validation.statements);
                response["errors"] = std::move(errors);
                response["warnings"] = std::move(warnings);
                return crow::response(200, response);
            }
            // Named as a template author writes them.
            static const std::map<std::string, std::string> kTemplateName = {
                {"cacheCatalog", "cache.catalog"}, {"cacheSchema", "cache.schema"},
                {"cacheTable", "cache.table"}, {"cacheSchedule", "cache.schedule"},
                {"cacheSnapshotId", "cache.snapshotId"}, {"cacheSnapshotTimestamp", "cache.snapshotTimestamp"},
                {"previousSnapshotId", "cache.previousSnapshotId"},
                {"previousSnapshotTimestamp", "cache.previousSnapshotTimestamp"},
                {"cursorColumn", "cache.cursorColumn"}, {"cursorType", "cache.cursorType"},
                {"primaryKeys", "cache.primaryKeys"}, {"cacheMode", "params.cacheMode"}};
            if (const auto it = cache_params.find(CacheManager::kWatermarkDroppedReasonKey);
                it != cache_params.end()) {
                response["watermark_dropped_reason"] = it->second;
                cache_params.erase(it);
            }
            crow::json::wvalue vars;
            for (const auto& [k, v] : cache_params) {
                const auto it = kTemplateName.find(k);
                vars[it != kTemplateName.end() ? it->second : k] = secrets.scrub(v);
            }
            response["variables"] = std::move(vars);
            return crow::response(200, response);
        }

        // Convert parameters to map
        std::map<std::string, std::string> params;
        for (const auto& param : json["parameters"]) {
            // Convert all parameter values to strings using JsonUtils
            std::string value = JsonUtils::valueToString(param);
            // See auth_params.hpp. These two routes were the copy without
            // the guard, and template/test EXECUTES what it renders.
            if (isReservedAuthKey(std::string(param.key()))) {
                continue;
            }
            params[param.key()] = value;
        }

        // Use SQLTemplateProcessor to expand the template
        auto sql_processor = std::make_shared<SQLTemplateProcessor>(config_manager_);
        
        // If validation only is requested, perform validation checks
        if (validate_only) {
            // The editor's UNSAVED text: validate this instead of the file on disk.
            // Nothing is written; it is only rendered and EXPLAINed.
            std::optional<std::string> candidate;
            if (json.has("template") && json["template"].t() == crow::json::type::String) {
                candidate = json["template"].s();
            }
            const auto secrets = collectTemplateSecrets(config_manager_.get(), *endpoint, params);
            crow::json::wvalue validation_response;
            crow::json::wvalue errors = crow::json::wvalue::list();
            crow::json::wvalue warnings = crow::json::wvalue::list();
            bool is_valid = true;
            
            try {
                // 1. Validate Mustache template syntax by attempting to load it
                std::string template_content;
                try {
                    template_content = candidate ? *candidate : sql_processor->loadTemplate(*endpoint);
                } catch (const std::exception& e) {
                    crow::json::wvalue error;
                    error["type"] = "template_load";
                    error["message"] = std::string("Failed to load template: ") + e.what();
                    errors[errors.size()] = std::move(error);
                    is_valid = false;
                }
                
                // 2. Validate variable references by creating template context
                if (is_valid) {
                    try {
                        std::map<std::string, std::string> context_params = params;
                        crow::mustache::context ctx = sql_processor->createTemplateContext(*endpoint, context_params);
                        
                        // Check if all variables in template are available in context
                        // This is a simple check - we could enhance this with proper Mustache parsing
                        std::regex var_regex(R"(\{\{([^}]+)\}\})");
                        std::sregex_iterator iter(template_content.begin(), template_content.end(), var_regex);
                        std::sregex_iterator end;
                        
                        for (; iter != end; ++iter) {
                            std::string var_name = (*iter)[1].str();
                            // Remove whitespace
                            var_name.erase(std::remove_if(var_name.begin(), var_name.end(), ::isspace), var_name.end());
                            
                            // Check if variable exists in context (simplified check)
                            std::string context_dump = ctx.dump();
                            if (context_dump.find("\"" + var_name + "\"") == std::string::npos) {
                                crow::json::wvalue warning;
                                warning["type"] = "undefined_variable";
                                warning["message"] = "Variable '" + var_name + "' may not be defined in context";
                                warning["variable"] = var_name;
                                warnings[warnings.size()] = std::move(warning);
                            }
                        }
                    } catch (const std::exception& e) {
                        crow::json::wvalue error;
                        error["type"] = "context_creation";
                        error["message"] = std::string("Failed to create template context: ") + e.what();
                        errors[errors.size()] = std::move(error);
                        is_valid = false;
                    }
                }
                
                // 3. Validate SQL by expanding and checking with DuckDB
                if (is_valid) {
                    try {
                        addCacheTargetParams(config_manager_, *endpoint, params);
                        // Rendered as the serving path renders it: typed
                        // `{{ params.x }}` become `?` placeholders. The pasted
                        // rendering reported `c = ALFKI` binder errors for SQL
                        // that never runs.
                        std::map<std::string, std::string> render_params = params;
                        const auto prepared =
                            candidate ? sql_processor->processTemplatePrepared(*candidate, *endpoint, render_params)
                                      : sql_processor->loadAndProcessTemplatePrepared(*endpoint, render_params);
                        validation_response["expanded"] =
                            secrets.withhold() ? std::string("<expansion withheld: a value this template "
                                                             "interpolates is a credential too short to "
                                                             "redact reliably>")
                                               : secrets.scrub(prepared.sql);

                        // Parsed per statement and prepared (bound and planned,
                        // as EXPLAIN does) inside a rolled-back transaction;
                        // nothing runs. A cached endpoint reading a cache that is
                        // not built yet is a warning, not an error.
                        auto db_manager = DatabaseManager::getInstance();
                        const auto cache_table = cacheTableOf(*config_manager_, *endpoint);
                        const auto validation = db_manager->validateScript(
                            prepared.sql, nullptr, cache_table ? &*cache_table : nullptr);
                        appendScriptIssues(validation, secrets, errors, warnings);
                        if (!validation.errors.empty()) {
                            is_valid = false;
                        }
                    } catch (const std::exception& e) {
                        crow::json::wvalue error;
                        error["type"] = "template_expansion";
                        error["message"] = secrets.scrub(std::string("Failed to expand template: ") + e.what());
                        errors[errors.size()] = std::move(error);
                        is_valid = false;
                    }
                }
                
            } catch (const std::exception& e) {
                crow::json::wvalue error;
                error["type"] = "validation";
                error["message"] = std::string("Validation error: ") + e.what();
                errors[errors.size()] = std::move(error);
                is_valid = false;
            }
            
            validation_response["valid"] = is_valid;
            validation_response["errors"] = std::move(errors);
            validation_response["warnings"] = std::move(warnings);
            
            return crow::response(200, validation_response);
        }
        
        // Normal template expansion. The cache target is server-owned context
        // the serving path adds before rendering (RequestHandler ->
        // DatabaseManager::executeQueryOnce); without it a cached endpoint
        // expanded to `FROM ..` - the SQL a user reads to debug a cached
        // endpoint was not the SQL that runs.
        addCacheTargetParams(config_manager_, *endpoint, params);
        // Rendered as the serving path renders it (executeQueryOnce): a typed
        // `{{ params.x }}` is a `?` placeholder whose value is BOUND, listed in
        // `bindings`. Pasting the value in showed SQL that never runs -
        // `ALFKI IS NULL OR`, and `( IS NULL OR c <= )` for an absent one.
        const std::map<std::string, std::string> bind_values = params;
        auto prepared = sql_processor->loadAndProcessTemplatePrepared(*endpoint, params);
        const std::string& expanded = prepared.sql;

        // Scrubbed, like the MCP dry-run preview - this IS the same payload.
        // A template interpolating `{{{ conn.password }}}` or
        // `{{{ env.API_KEY }}}` hands the credential to whoever asked, through
        // a route that needs no _dryRun flag. Where a value is too short to
        // replace safely the expansion is withheld rather than mangled.
        const auto secrets = collectTemplateSecrets(config_manager_.get(), *endpoint, params);
        crow::json::wvalue response;
        response["expanded"] = secrets.withhold()
                                   ? std::string("<expansion withheld: a value this template "
                                                 "interpolates is a credential too short to "
                                                 "redact reliably>")
                                   : secrets.scrub(expanded);
        response["bindings"] = MCPDryRun::bindingsJson(prepared.bindings, bind_values,
                                                       endpoint->request_fields, secrets);

        // Include variable metadata if requested.
        //
        // The same policy as `expanded` above. This block hands back the
        // context's raw values - conn.* and env.* included - three lines
        // after the scrub that exists to keep them out of the response, so
        // `?include_variables=1` was a complete way around it. Each value now
        // goes through the same TemplateSecrets scrub, which redacts a
        // credential and leaves ordinary configuration visible.
        const auto redact = [&secrets](const std::string& value) {
            return secrets.withhold() ? std::string("<redacted>")
                                      : secrets.scrub(value);
        };
        if (include_variables) {
            // Create a copy of params for context creation (since it might be modified)
            std::map<std::string, std::string> context_params = params;
            crow::mustache::context ctx = sql_processor->createTemplateContext(*endpoint, context_params);
            
            // Convert context to JSON for variables metadata
            crow::json::wvalue variables;
            
            // Extract variables from the context
            std::string context_dump = ctx.dump();
            auto context_json = crow::json::load(context_dump);
            
            if (context_json) {
                // Add request parameters (from params namespace)
                if (context_json.has("params")) {
                    crow::json::wvalue request_vars;
                    for (const auto& key : context_json["params"].keys()) {
                        crow::json::wvalue var_info;
                        var_info["type"] = "string";
                        var_info["value"] = redact(context_json["params"][key].s());
                        var_info["source"] = "request";
                        request_vars[key] = std::move(var_info);
                    }
                    variables["params"] = std::move(request_vars);
                }
                
                // Add connection variables
                if (context_json.has("conn")) {
                    crow::json::wvalue conn_vars;
                    for (const auto& key : context_json["conn"].keys()) {
                        crow::json::wvalue var_info;
                        var_info["type"] = "string";
                        var_info["value"] = redact(context_json["conn"][key].s());
                        var_info["source"] = "connection";
                        conn_vars[key] = std::move(var_info);
                    }
                    variables["conn"] = std::move(conn_vars);
                }
                
                // Add environment variables
                if (context_json.has("env")) {
                    crow::json::wvalue env_vars;
                    for (const auto& key : context_json["env"].keys()) {
                        crow::json::wvalue var_info;
                        var_info["type"] = "string";
                        var_info["value"] = redact(context_json["env"][key].s());
                        var_info["source"] = "environment";
                        env_vars[key] = std::move(var_info);
                    }
                    variables["env"] = std::move(env_vars);
                }
                
                // Add cache variables
                if (context_json.has("cache")) {
                    crow::json::wvalue cache_vars;
                    for (const auto& key : context_json["cache"].keys()) {
                        crow::json::wvalue var_info;
                        var_info["type"] = "string";
                        var_info["value"] = redact(context_json["cache"][key].s());
                        var_info["source"] = "cache";
                        cache_vars[key] = std::move(var_info);
                    }
                    variables["cache"] = std::move(cache_vars);
                }
            }
            
            response["variables"] = std::move(variables);
        }

        return crow::response(200, response);
    } catch (const std::invalid_argument& e) {
        // RequireValidMustacheSyntax: an unbalanced template is a 400 naming
        // file:line, not an internal error.
        return jsonError(400, e.what());
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

crow::response TemplateHandler::testTemplate(const crow::request& req, const std::string& path) {
    try {
        auto json = crow::json::load(req.body);
        if (!json || !json.has("parameters")) {
            return crow::response(400, "Invalid JSON: missing 'parameters' field");
        }

        // Find the endpoint
        auto endpoint = config_manager_->getEndpointForKey(path);
        if (!endpoint) {
            return crow::response(404, "Endpoint not found");
        }

        if (endpoint->connection.empty()) {
            return crow::response(400, "Endpoint has no database connection configured");
        }

        // Convert parameters to map
        std::map<std::string, std::string> params;
        for (const auto& param : json["parameters"]) {
            // Convert all parameter values to strings using JsonUtils
            std::string value = JsonUtils::valueToString(param);
            // See auth_params.hpp. These two routes were the copy without
            // the guard, and template/test EXECUTES what it renders.
            if (isReservedAuthKey(std::string(param.key()))) {
                continue;
            }
            params[param.key()] = value;
        }

        // Run the endpoint's OWN validators before executing.
        //
        // This route executes the rendered template, and it used to copy
        // `parameters` straight into the map - so it bypassed the
        // RequestValidator layer that CLAUDE.md calls the first line of
        // defence. A template of the documented shape
        // `AND status = '{{{ params.status }}}'` would therefore execute
        // whatever the caller put in `status`, while the same value sent to
        // the endpoint itself would be rejected. A test route must not be a
        // way around an endpoint's own constraints.
        {
            RequestValidator validator;
            const auto errors =
                validator.validateRequestParameters(endpoint->request_fields, params);
            if (!errors.empty()) {
                crow::json::wvalue body;
                body["success"] = false;
                body["error"] = "Parameter validation failed";
                crow::json::wvalue::list details;
                for (const auto& error : errors) {
                    crow::json::wvalue detail;
                    detail["field"] = error.fieldName;
                    detail["message"] = error.errorMessage;
                    details.push_back(std::move(detail));
                }
                body["validation_errors"] = std::move(details);
                return crow::response(400, body.dump());
            }
        }

        // Get database manager instance
        auto db_manager = DatabaseManager::getInstance();

        // Execute the query with a limit for safety
        params["limit"] = "10";  // Force a limit for test queries
        params["offset"] = "0";
        
        try {
            auto result = db_manager->executeQuery(*endpoint, params);
            
            // Create base response
            crow::json::wvalue response;
            response["success"] = true;
            
            // Handle empty result case
            if (result.data.t() == crow::json::type::Null || result.data.size() == 0) {
                response["columns"] = std::vector<std::string>();
                response["rows"] = std::vector<crow::json::wvalue>();
                return crow::response(200, response);
            }

            response["rows"] = std::move(result.data);
            response["columns"] = response["rows"][0].keys();
            
            return crow::response(200, response);

        } catch (const std::exception& e) {
            // Scrubbed. This is the one config-service route that EXECUTES
            // the rendered template, so DuckDB's message quotes the statement
            // verbatim - including whatever it interpolated from conn.* and
            // env.*. expandTemplate 180 lines above already withholds or
            // scrubs its output, and getEnvironmentVariables redacts at this
            // same privilege level on the grounds that a token holder never
            // needed the key itself; leaving this one raw made the policy
            // inconsistent inside one file, on its most dangerous route.
            return crow::response(400, publicErrorMessage(
                "SQL execution error", e.what(),
                collectTemplateSecrets(config_manager_.get(), *endpoint, params)));
        }
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

// Slug-based wrapper methods for TemplateHandler
crow::response TemplateHandler::getEndpointTemplateBySlug(const crow::request& req, const std::string& slug) {
    const auto endpoint = findEndpointBySlug(config_manager_, slug);
    if (!endpoint) {
        return crow::response(404, "Endpoint not found");
    }
    return getEndpointTemplate(req, endpoint->getName());
}

crow::response TemplateHandler::updateEndpointTemplateBySlug(const crow::request& req, const std::string& slug) {
    const auto endpoint = findEndpointBySlug(config_manager_, slug);
    if (!endpoint) {
        return crow::response(404, "Endpoint not found");
    }
    return updateEndpointTemplate(req, endpoint->getName());
}

crow::response TemplateHandler::expandTemplateBySlug(const crow::request& req, const std::string& slug) {
    const auto endpoint = findEndpointBySlug(config_manager_, slug);
    if (!endpoint) {
        return crow::response(404, "Endpoint not found");
    }
    return expandTemplate(req, endpoint->getName());
}

crow::response TemplateHandler::testTemplateBySlug(const crow::request& req, const std::string& slug) {
    const auto endpoint = findEndpointBySlug(config_manager_, slug);
    if (!endpoint) {
        return crow::response(404, "Endpoint not found");
    }
    return testTemplate(req, endpoint->getName());
}

crow::response CacheConfigHandler::getCacheConfig(const crow::request& req, const std::string& path) {
    try {
        auto endpoint = config_manager_->getEndpointForKey(path);
        if (!endpoint) {
            return crow::response(404, "Endpoint not found");
        }

        crow::json::wvalue response;
        response["enabled"] = endpoint->cache.enabled;
        response["table"] = endpoint->cache.table;
        response["schema"] = endpoint->cache.schema;
        if (endpoint->cache.schedule) {
            response["schedule"] = endpoint->cache.schedule.value();
        }
        if (!endpoint->cache.primary_keys.empty()) {
            auto pk_list = crow::json::wvalue::list();
            for (const auto& pk : endpoint->cache.primary_keys) {
                pk_list.push_back(pk);
            }
            response["primary-key"] = std::move(pk_list);
        }
        if (endpoint->cache.cursor) {
            crow::json::wvalue cursor;
            cursor["column"] = endpoint->cache.cursor->column;
            cursor["type"] = endpoint->cache.cursor->type;
            response["cursor"] = std::move(cursor);
        }
        if (endpoint->cache.rollback_window) {
            response["rollback-window"] = endpoint->cache.rollback_window.value();
        }
        if (endpoint->cache.retention.keep_last_snapshots || endpoint->cache.retention.max_snapshot_age) {
            crow::json::wvalue retention;
            if (endpoint->cache.retention.keep_last_snapshots) {
                retention["keep-last-snapshots"] = static_cast<int64_t>(endpoint->cache.retention.keep_last_snapshots.value());
            }
            if (endpoint->cache.retention.max_snapshot_age) {
                retention["max-snapshot-age"] = endpoint->cache.retention.max_snapshot_age.value();
            }
            response["retention"] = std::move(retention);
        }
        if (endpoint->cache.delete_handling) {
            response["delete-handling"] = endpoint->cache.delete_handling.value();
        }
        if (endpoint->cache.template_file) {
            response["template-file"] = endpoint->cache.template_file.value();
        }

        return crow::response(200, response);
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

namespace {

// ducklake.read-only: this instance serves a catalog another process writes.
// The message names what to do INSTEAD for this operation: "refresh it from the
// writer" was the answer to a garbage collection and a template edit as well.
crow::response readOnlyCacheResponse(const std::string& advice) {
    crow::json::wvalue body;
    body["error"] = "read_only_cache";
    body["message"] = "The DuckLake catalog is attached read-only (ducklake.read-only); " + advice;
    crow::response response(409, body);
    response.set_header("Content-Type", "application/json");
    return response;
}

} // namespace

crow::response CacheConfigHandler::updateCacheConfig(const crow::request& req, const std::string& path) {
    if (config_manager_->getDuckLakeConfig().read_only) {
        return readOnlyCacheResponse("change the cache configuration on the writer instance");
    }
    try {
        auto json = crow::json::load(req.body);
        if (!json) {
            return crow::response(400, "Invalid JSON");
        }

        auto endpoint = config_manager_->getEndpointForKey(path);
        if (!endpoint) {
            return crow::response(404, "Endpoint not found");
        }

        // Edit a COPY and install it atomically. The previous version
        // const_cast away the constness of the snapshot and wrote through it,
        // so a reader iterating the same snapshot could observe a half-applied
        // cache config - table updated, schema not yet - and the write bypassed
        // the copy-on-write discipline the snapshot exists to provide.
        EndpointConfig updated = *endpoint;
        CacheConfig& cache = updated.cache;
        // A partial update (`{"schedule": "10m"}`) leaves `enabled` alone; reading
        // the missing key threw "cannot find key" and failed the whole request.
        const bool enabled = json.has("enabled") ? json["enabled"].b() : cache.enabled;
        if (enabled) {
            auto table_key = json.has("table") ? json["table"].s() : updated.cache.table;
            auto schema_key = json.has("schema") ? json["schema"].s() : updated.cache.schema;
            cache.enabled = true;
            cache.table = table_key;
            cache.schema = schema_key;
        } else {
            cache.enabled = false;
        }
        if (json.has("schedule")) {
            const std::string new_schedule = json["schedule"].s();
            if (!TimeInterval::parseInterval(new_schedule)) {
                return crow::response(400, "Invalid cache schedule '" + new_schedule +
                                           "': expected <positive number>[s|m|h|d], e.g. 30s, 5m, 6h, 1d");
            }
            cache.schedule = new_schedule;
        }
        if (json.has("primary-key")) {
            cache.primary_keys.clear();
            for (const auto& pk : json["primary-key"]) {
                cache.primary_keys.push_back(pk.s());
            }
        }
        if (json.has("cursor")) {
            CacheConfig::CursorConfig cursor;
            cursor.column = json["cursor"]["column"].s();
            cursor.type = json["cursor"]["type"].s();
            cache.cursor = cursor;
        }
        if (json.has("rollback-window")) {
            cache.rollback_window = json["rollback-window"].s();
        }
        if (json.has("retention")) {
            const auto& retention = json["retention"];
            if (retention.has("keep-last-snapshots")) {
                cache.retention.keep_last_snapshots = static_cast<std::size_t>(retention["keep-last-snapshots"].i());
            }
            if (retention.has("max-snapshot-age")) {
                cache.retention.max_snapshot_age = retention["max-snapshot-age"].s();
            }
        }
        if (json.has("delete-handling")) {
            cache.delete_handling = json["delete-handling"].s();
        }
        if (json.has("template-file")) {
            const auto problem = config_manager_->TemplatePathProblem(json["template-file"].s());
            if (!problem.empty()) {
                return crow::response(400, problem);
            }
            cache.template_file = json["template-file"].s();
        }

        if (!config_manager_->replaceEndpoint(updated)) {
            return crow::response(404, "Endpoint not found");
        }

        return crow::response(200);
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

crow::response TemplateHandler::getCacheTemplate(const crow::request& req, const std::string& path) {
    try {
        auto endpoint = config_manager_->getEndpointForKey(path);
        if (!endpoint) {
            return crow::response(404, "Endpoint not found");
        }
        if (!endpoint->cache.enabled || !endpoint->cache.template_file) {
            return crow::response(400, "Cache template not configured for this endpoint");
        }

        auto cache_path = resolveTemplatePath(endpoint->cache.template_file.value());
        std::ifstream file(cache_path);
        if (!file.is_open()) {
            return crow::response(500, "Could not open cache template file: " + cache_path.string());
        }

        std::stringstream buffer;
        buffer << file.rdbuf();

        crow::json::wvalue response;
        response["template"] = buffer.str();
        return crow::response(200, response);
    } catch (const std::invalid_argument& e) {
        return crow::response(400, e.what());
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

crow::response TemplateHandler::updateCacheTemplate(const crow::request& req, const std::string& path) {
    // A replica shares the populate SQL with the writer (same config tree, often
    // the same volume); overwriting it from here changed what the WRITER runs.
    if (config_manager_->getDuckLakeConfig().read_only) {
        return readOnlyCacheResponse("the populate template belongs to the writer; edit it there");
    }
    try {
        auto json = crow::json::load(req.body);
        if (!json || !json.has("template")) {
            return crow::response(400, "Invalid JSON: missing 'template' field");
        }

        auto endpoint = config_manager_->getEndpointForKey(path);
        if (!endpoint) {
            return crow::response(404, "Endpoint not found");
        }
        if (!endpoint->cache.enabled || !endpoint->cache.template_file) {
            return crow::response(400, "Cache template not configured for this endpoint");
        }

        if (const auto syntax = CheckMustacheSyntax(json["template"].s())) {
            return jsonError(400, "Invalid template " +
                                      FormatMustacheSyntaxError(*syntax, endpoint->cache.template_file.value()));
        }

        auto cache_path = resolveTemplatePath(endpoint->cache.template_file.value());
        std::ofstream file(cache_path);
        if (!file.is_open()) {
            return crow::response(500, "Could not open cache template file for writing: " + cache_path.string());
        }

        file << json["template"].s();
        if (!file.good()) {
            return crow::response(500, "Error writing template to file");
        }

        return crow::response(200);
    } catch (const std::invalid_argument& e) {
        return crow::response(400, e.what());
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

crow::response CacheConfigHandler::refreshCache(const crow::request& req, const std::string& path) {
    if (config_manager_->getDuckLakeConfig().read_only) {
        return readOnlyCacheResponse("refresh it from the writer, e.g. `flapi cache refresh`");
    }
    // JSON both ways. The success answer used to be an empty 200 and the errors
    // plain text, so `flapii cache refresh` had nothing to print.
    const auto refreshError = [](int status, const std::string& error, const std::string& reason) {
        crow::json::wvalue body;
        body["error"] = error;
        if (!reason.empty()) {
            body["reason"] = reason;
        }
        crow::response res(status, body);
        res.set_header("Content-Type", "application/json");
        return res;
    };
    try {
        // Find the endpoint
        auto endpoint = config_manager_->getEndpointForKey(path);
        if (!endpoint) {
            return refreshError(404, "Endpoint not found", path);
        }

        if (!endpoint->cache.enabled) {
            return refreshError(400, "Cache is not enabled for this endpoint", path);
        }

        auto db_manager = DatabaseManager::getInstance();
        auto cache_manager = db_manager->getCacheManager();
        if (!cache_manager) {
            return refreshError(500, "Cache manager is not initialized", "");
        }

        // Prepare empty parameters map for cache refresh
        std::map<std::string, std::string> params;

        // `?full=true` forces a full rebuild of an incremental cache: the watermark is
        // erased (the template's full-load branch runs) and the table is rebuilt in the
        // same transaction. Anything else - absent, `false`, `0` - is the usual refresh.
        CacheManager::RefreshOptions options;
        if (const char* full = req.url_params.get("full")) {
            const std::string value(full);
            options.full = value == "true" || value == "1" || value.empty();
        }

        bool ran = false;
        try {
            ran = cache_manager->refreshCache(config_manager_, *endpoint, params, options);
        } catch (const std::exception& e) {
            if (DatabaseHealth::IsFatal()) {
                return databaseFatalResponse();
            }
            return refreshError(400, "Cache refresh failed", e.what());
        }

        crow::json::wvalue body;
        body["endpoint"] = endpoint->urlPath.empty() ? endpoint->getName() : endpoint->urlPath;
        body["full"] = options.full;
        if (!ran) {
            // Another refresh of the same table (warm-up, the scheduler) is in
            // flight; it was not run twice.
            body["status"] = "already_running";
        } else {
            body["status"] = "refreshed";
            // The stats this refresh just recorded (CacheManager::recordSyncEvent).
            const std::string schema = endpoint->cache.schema.empty() ? "main" : endpoint->cache.schema;
            for (const auto& stats : cache_manager->getRefreshStats()) {
                if (stats.catalog != config_manager_->getDuckLakeConfig().alias || stats.schema != schema ||
                    stats.table != endpoint->cache.table) {
                    continue;
                }
                body["mode"] = stats.last_sync_type;
                // What the refresh did: a forced rebuild, a full-mode cache, or an
                // incremental cache's bootstrap all load the whole source.
                body["full"] = stats.last_full;
                if (stats.last_duration_ms) {
                    body["duration_ms"] = *stats.last_duration_ms;
                }
                if (stats.last_rows_affected) {
                    body["rows_affected"] = *stats.last_rows_affected;
                }
                if (stats.last_snapshot_id) {
                    body["snapshot_id"] = *stats.last_snapshot_id;
                }
                if (stats.last_warning) {
                    body["warning"] = *stats.last_warning;
                }
                break;
            }
        }
        crow::response res(200, body);
        res.set_header("Content-Type", "application/json");
        return res;
    } catch (const std::exception& e) {
        return refreshError(500, "Internal server error", e.what());
    }
}

crow::response CacheConfigHandler::dropCacheTable(const crow::request& req) {
    const auto answer = [](int status, crow::json::wvalue body) {
        crow::response res(status, body);
        res.set_header("Content-Type", "application/json");
        return res;
    };
    const auto error = [&answer](int status, const std::string& code, const std::string& message) {
        crow::json::wvalue body;
        body["error"] = code;
        body["message"] = message;
        return answer(status, std::move(body));
    };
    if (config_manager_->getDuckLakeConfig().read_only) {
        return readOnlyCacheResponse("drop the table from the writer instance");
    }
    if (!config_manager_->getDuckLakeConfig().enabled) {
        return error(400, "ducklake_disabled", "DuckLake is not enabled (ducklake.enabled)");
    }
    auto json = crow::json::load(req.body);
    if (!json || json.t() != crow::json::type::Object || !json.has("table") ||
        json["table"].t() != crow::json::type::String || json["table"].s().size() == 0) {
        return error(400, "invalid_request", R"(expected {"schema": "<schema>", "table": "<table>"})");
    }
    const std::string table = json["table"].s();
    std::string schema = "main";
    if (json.has("schema")) {
        if (json["schema"].t() != crow::json::type::String) {
            return error(400, "invalid_request", "`schema` must be a string");
        }
        schema = json["schema"].s();
    }
    if (schema == "audit") {
        return error(409, "protected_table", "the audit schema is flapi's own and cannot be dropped");
    }
    // Only an OBSOLETE table: one no configured endpoint caches into. Dropping a
    // live cache would turn its endpoint into a 503 until the next refresh.
    for (const auto& endpoint : *config_manager_->getEndpoints()) {
        if (!endpoint.cache.enabled || endpoint.cache.table.empty()) {
            continue;
        }
        const std::string ep_schema = endpoint.cache.schema.empty() ? "main" : endpoint.cache.schema;
        if (ep_schema == schema && endpoint.cache.table == table) {
            return error(409, "cache_table_in_use",
                         schema + "." + table + " is the cache table of " +
                             (endpoint.urlPath.empty() ? endpoint.getName() : endpoint.urlPath) +
                             "; only a table no endpoint uses can be dropped");
        }
    }
    auto db_manager = DatabaseManager::getInstance();
    auto cache_manager = db_manager ? db_manager->getCacheManager() : nullptr;
    if (!cache_manager) {
        return error(500, "internal_error", "Cache manager is not initialized");
    }
    try {
        const auto result = cache_manager->dropObsoleteCacheTable(config_manager_, schema, table);
        if (result.status == "not_found") {
            return error(404, "table_not_found", result.message);
        }
        crow::json::wvalue body;
        body["status"] = result.status;
        body["schema"] = schema;
        body["table"] = table;
        body["expired_snapshots"] = static_cast<int64_t>(result.expired_snapshots);
        body["files_deleted"] = static_cast<int64_t>(result.files_deleted);
        body["bytes_freed"] = result.bytes_freed;
        body["message"] = result.message;
        if (!result.warning.empty()) {
            body["warning"] = result.warning;
        }
        if (!result.pinned_by.empty()) {
            crow::json::wvalue pinned = crow::json::wvalue::list();
            for (std::size_t i = 0; i < result.pinned_by.size(); ++i) {
                pinned[i] = result.pinned_by[i];
            }
            body["pinned_by"] = std::move(pinned);
        }
        return answer(200, std::move(body));
    } catch (const std::exception& e) {
        return error(500, "drop_failed", e.what());
    }
}

crow::response CacheConfigHandler::compactCache(const crow::request& req) {
    const auto answer = [](int status, crow::json::wvalue body) {
        crow::response res(status, body);
        res.set_header("Content-Type", "application/json");
        return res;
    };
    const auto error = [&answer](int status, const std::string& code, const std::string& message) {
        crow::json::wvalue body;
        body["error"] = code;
        body["message"] = message;
        return answer(status, std::move(body));
    };
    if (config_manager_->getDuckLakeConfig().read_only) {
        return readOnlyCacheResponse("compaction rewrites files; run it on the writer");
    }
    if (!config_manager_->getDuckLakeConfig().enabled) {
        return error(400, "ducklake_disabled", "DuckLake is not enabled (ducklake.enabled)");
    }
    std::string schema;
    std::string table;
    if (!req.body.empty()) {
        auto json = crow::json::load(req.body);
        if (!json || json.t() != crow::json::type::Object) {
            return error(400, "invalid_request", R"(expected {} or {"endpoint": "<url-path>", "method": "GET"})");
        }
        if (json.has("endpoint")) {
            if (json["endpoint"].t() != crow::json::type::String) {
                return error(400, "invalid_request", "`endpoint` must be a string");
            }
            const std::string method =
                json.has("method") && json["method"].t() == crow::json::type::String ? std::string(json["method"].s())
                                                                                      : std::string();
            const auto lookup = FindCachedEndpointForPath(*config_manager_, json["endpoint"].s(), method);
            if (!lookup.endpoint) {
                return error(lookup.found ? 400 : 404, lookup.found ? "no_cache" : "endpoint_not_found", lookup.error);
            }
            schema = lookup.endpoint->cache.schema.empty() ? "main" : lookup.endpoint->cache.schema;
            table = lookup.endpoint->cache.table;
        }
    }
    auto db_manager = DatabaseManager::getInstance();
    auto cache_manager = db_manager ? db_manager->getCacheManager() : nullptr;
    if (!cache_manager) {
        return error(500, "internal_error", "Cache manager is not initialized");
    }
    try {
        const auto result = cache_manager->performCompaction(config_manager_, schema, table);
        crow::json::wvalue body;
        body["status"] = result.status;
        body["duration_ms"] = result.duration_ms;
        if (result.files_rewritten) {
            body["files_rewritten"] = *result.files_rewritten;
        }
        if (!table.empty()) {
            body["schema"] = schema;
            body["table"] = table;
        }
        body["message"] = result.message;
        if (result.status == "error" && DatabaseHealth::IsFatal()) {
            return databaseFatalResponse();
        }
        return answer(result.status == "error" ? 500 : 200, std::move(body));
    } catch (const std::exception& e) {
        if (DatabaseHealth::IsFatal()) {
            return databaseFatalResponse();
        }
        return error(500, "compaction_failed", e.what());
    }
}

crow::response CacheConfigHandler::performGarbageCollection(const crow::request& req, const std::string& path) {
    if (config_manager_->getDuckLakeConfig().read_only) {
        return readOnlyCacheResponse("snapshots are expired by the writer: its refreshes apply the retention "
                                     "policy, or run the garbage collection against the writer");
    }
    try {
        // Find the endpoint
        auto endpoint = config_manager_->getEndpointForKey(path);
        if (!endpoint) {
            return crow::response(404, "Endpoint not found");
        }

        if (!endpoint->cache.enabled) {
            return crow::response(400, "Cache is not enabled for this endpoint");
        }

        auto db_manager = DatabaseManager::getInstance();
        auto cache_manager = db_manager->getCacheManager();
        if (!cache_manager) {
            return crow::response(500, "Cache manager is not initialized");
        }

        try {
            // JSON, with what it did. It used to answer the text "Garbage collection
            // completed" whether it expired anything or not - including with no
            // retention policy at all, when it never does.
            const auto result = cache_manager->performGarbageCollection(config_manager_, *endpoint, {});
            crow::json::wvalue body;
            body["status"] = result.status;
            body["expired_snapshots"] = static_cast<int64_t>(result.expired_snapshots);
            body["files_deleted"] = static_cast<int64_t>(result.files_deleted);
            body["bytes_freed"] = result.bytes_freed;
            body["message"] = result.message;
            if (!result.warning.empty()) {
                body["warning"] = result.warning;
            }
            crow::response res(result.status == "error" ? 500 : 200, body);
            res.set_header("Content-Type", "application/json");
            return res;
        } catch (const std::exception& e) {
            return crow::response(400, std::string("Garbage collection failed: ") + e.what());
        }
    } catch (const std::exception& e) {
        return crow::response(500, std::string("Internal server error: ") + e.what());
    }
}

std::filesystem::path TemplateHandler::resolveTemplatePath(const std::string& source) const {
    // The one boundary every template/cache-template read and write passes: refuse
    // anything outside the templates directory (absolute, `../`, or via a symlink).
    if (config_manager_) {
        const auto problem = config_manager_->TemplatePathProblem(source);
        if (!problem.empty()) {
            throw std::invalid_argument(problem);
        }
    }

    std::filesystem::path template_path(source);
    if (template_path.is_absolute()) {
        return template_path;
    }

    if (!config_manager_) {
        return template_path;
    }

    std::filesystem::path base_path(config_manager_->getTemplatePath());
    return (base_path / template_path).lexically_normal();
}

crow::response SchemaHandler::getSchema(const crow::request& req) {
    try {
        CROW_LOG_DEBUG << "SchemaHandler::getSchema called";
        
        // Parse query parameters
        auto url_params = req.url_params;
        bool tables_only = url_params.get("tables") != nullptr;
        bool connections_only = url_params.get("connections") != nullptr;
        bool completion_format = url_params.get("format") && std::string(url_params.get("format")) == "completion";
        std::string specific_connection = url_params.get("connection") ? url_params.get("connection") : "";
        
        CROW_LOG_DEBUG << "Schema query params - tables_only: " << tables_only 
                      << ", connections_only: " << connections_only 
                      << ", completion_format: " << completion_format
                      << ", connection: " << specific_connection;
        
        // Get database manager instance
        auto db_manager = DatabaseManager::getInstance();
        if (!db_manager) {
            CROW_LOG_ERROR << "DatabaseManager instance is null";
            return crow::response(500, "Internal server error: Database manager not initialized");
        }

        // For now, return a simple response based on what's requested
        crow::json::wvalue response;
        
        if (connections_only) {
            // The connections flapi.yaml declares. This was a TODO that always
            // answered [], so `flapii schema connections` reported none on every
            // server. Names and a type hint only: properties can hold
            // credentials and are never returned here.
            response["connections"] = crow::json::wvalue::list();
            static const std::regex attach_type(R"(\(\s*TYPE\s+([A-Za-z_]+))", std::regex::icase);
            std::vector<std::string> names;
            for (const auto& [name, _conn] : config_manager_->getConnections()) {
                names.push_back(name);
            }
            std::sort(names.begin(), names.end());
            size_t idx = 0;
            for (const auto& name : names) {
                const auto& conn = config_manager_->getConnections().at(name);
                std::string type = "duckdb";
                std::smatch match;
                if (std::regex_search(conn.init, match, attach_type)) {
                    type = match[1].str();
                    std::transform(type.begin(), type.end(), type.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                } else if (auto it = conn.properties.find("path"); it != conn.properties.end()) {
                    const auto ext = std::filesystem::path(it->second).extension().string();
                    type = ext.empty() ? "file" : ext.substr(1);
                }
                crow::json::wvalue entry;
                entry["name"] = name;
                entry["type"] = type;
                entry["status"] = "configured";
                response["connections"][idx++] = std::move(entry);
            }
            return crow::response(200, response);
        }

        if (tables_only) {
            // duckdb_tables() calls the catalog `database_name`; there is no
            // `database` column, so this query was a Binder Error on every
            // server and `flapii schema tables` always failed.
            //
            // Tables only, like the full schema below. duckdb_views() is not
            // used: it parses every view of every attached database, and one
            // foreign view (northwind's SQLite views quote names as [Order
            // Details]) fails the whole query with a Parser Error.
            std::string tables_query = R"SQL(
                SELECT database_name, schema_name, table_name, 'table' AS table_type
                FROM duckdb_tables()
                WHERE NOT internal AND database_name NOT IN ('system', 'temp')
            )SQL";
            if (!specific_connection.empty()) {
                std::string escaped;
                for (const char c : specific_connection) {
                    escaped += c;
                    if (c == '\'') {
                        escaped += '\'';
                    }
                }
                tables_query += " AND database_name = '" + escaped + "'";
            }
            tables_query += " ORDER BY database_name, schema_name, table_name";

            try {
                CROW_LOG_DEBUG << "Executing tables query for schema introspection";
                auto tables_result = db_manager->executeQuery(tables_query, {}, false);
                response["tables"] = crow::json::wvalue::list();
                const auto& connections = config_manager_->getConnections();

                if (tables_result.data.t() != crow::json::type::Null) {
                    size_t idx = 0;
                    for (size_t i = 0; i < tables_result.data.size(); ++i) {
                        const auto row = crow::json::load(tables_result.data[i].dump());
                        const std::string database = row["database_name"].s();
                        const std::string schema = row["schema_name"].s();
                        const std::string table = row["table_name"].s();

                        crow::json::wvalue table_entry;
                        table_entry["name"] = table;
                        table_entry["schema"] = schema;
                        table_entry["database"] = database;
                        table_entry["type"] = row["table_type"].s();
                        // An attached database named like a connection is that connection's.
                        table_entry["connection"] = connections.count(database) ? database : std::string();
                        table_entry["qualified_name"] = database + "." + schema + "." + table;
                        response["tables"][idx++] = std::move(table_entry);
                    }
                }
            } catch (const std::exception& e) {
                CROW_LOG_ERROR << "Tables query error: " << e.what();
                return crow::response(500, std::string("Failed to query tables: ") + e.what());
            }

            return crow::response(200, response);
        }

        // Use DuckDB system functions instead of information_schema to avoid parser issues
        // Query tables and columns using duckdb_tables() and duckdb_columns()
        const std::string query = R"SQL(
                SELECT 
                COALESCE(t.database_name, 'main') as schema_name,
                    t.table_name,
                0 as is_view,
                    c.column_name,
                    c.data_type,
                CASE WHEN c.is_nullable THEN 1 ELSE 0 END as is_nullable
            FROM duckdb_tables() t
            LEFT JOIN duckdb_columns() c 
                ON t.database_name = c.database_name
                AND t.schema_name = c.schema_name
                    AND t.table_name = c.table_name
            WHERE COALESCE(t.database_name, 'main') NOT IN ('system', 'temp')
            ORDER BY t.database_name, t.table_name, c.column_index
        )SQL";

        QueryResult result;
        try {
            CROW_LOG_DEBUG << "Executing full schema introspection query";
            result = db_manager->executeQuery(query, {}, false);
            CROW_LOG_DEBUG << "Schema query executed successfully, processing " << result.data.size() << " rows";
        } catch (const std::exception& e) {
            CROW_LOG_ERROR << "========================================";
            CROW_LOG_ERROR << "SCHEMA INTROSPECTION FAILED";
            CROW_LOG_ERROR << "========================================";
            CROW_LOG_ERROR << "Error type: " << typeid(e).name();
            CROW_LOG_ERROR << "Error message: " << e.what();
            CROW_LOG_ERROR << "Query attempted:";
            CROW_LOG_ERROR << query;
            CROW_LOG_ERROR << "========================================";
            // Return empty response with error message for debugging
            response["error"] = std::string("Failed to query schema: ") + e.what();
            response["message"] = "Schema introspection failed. This may be due to DuckDB information_schema limitations or database connection issues.";
            response["query"] = query;
            return crow::response(200, response);
        }
    
        // Process results into the desired JSON structure
        if (result.data.t() == crow::json::type::Null) {
            return crow::response(200, response);
        }

        // Build the tree structure from flat results
        for (size_t i = 0; i < result.data.size(); ++i) {
            const auto& row = crow::json::load(result.data[i].dump());
            
            // Safely extract string values, handling NULL
            auto safe_string = [](const crow::json::rvalue& val) -> std::string {
                if (val.t() == crow::json::type::String) {
                    return val.s();
                }
                return "";
            };
            
            // Safely extract boolean values, handling NULL and integers (0/1)
            auto safe_bool = [](const crow::json::rvalue& val) -> bool {
                if (val.t() == crow::json::type::True) {
                    return true;
                } else if (val.t() == crow::json::type::False) {
                    return false;
                } else if (val.t() == crow::json::type::Number) {
                    return val.i() != 0;
                }
                return false;
            };
            
            const std::string schema_name = safe_string(row["schema_name"]);
            const std::string table_name = safe_string(row["table_name"]);
            const std::string column_name = safe_string(row["column_name"]);
            const std::string data_type = safe_string(row["data_type"]);
            const bool is_nullable = safe_bool(row["is_nullable"]);
            const bool is_view = safe_bool(row["is_view"]);

            // Initialize schema if not exists
            auto schema_names = response.keys();
            if (std::find(schema_names.begin(), schema_names.end(), schema_name) == schema_names.end()) {
                response[schema_name]["tables"] = crow::json::wvalue();
            }

            if (table_name.size() == 0) {
                continue;
            }

            // Initialize table if not exists
            auto table_names = response[schema_name]["tables"].keys();
            if (std::find(table_names.begin(), table_names.end(), table_name) == table_names.end()) {
                response[schema_name]["tables"][table_name]["is_view"] = is_view;
                response[schema_name]["tables"][table_name]["columns"] = crow::json::wvalue();
            }

            if (column_name.size() == 0) {
                continue;
            }

            // Add column info
            auto& columns = response[schema_name]["tables"][table_name]["columns"];
            columns[column_name]["type"] = data_type;
            columns[column_name]["nullable"] = is_nullable;
        }

        // If completion format is requested, transform the response
        if (completion_format) {
            crow::json::wvalue completion_response;
            crow::json::wvalue tables_list = crow::json::wvalue::list();
            crow::json::wvalue columns_list = crow::json::wvalue::list();
            
            // Flatten tables and columns for completion
            for (const auto& schema_name : response.keys()) {
                const auto& schema_obj = response[schema_name];
                auto schema_keys = schema_obj.keys();
                if (std::find(schema_keys.begin(), schema_keys.end(), "tables") != schema_keys.end()) {
                    for (const auto& table_name : schema_obj["tables"].keys()) {
                        const auto& table_obj = schema_obj["tables"][table_name];
                        
                        // Add table entry
                        crow::json::wvalue table_entry;
                        table_entry["name"] = table_name;
                        table_entry["schema"] = schema_name;
                        
                        // Check if it's a view
                        auto table_keys = table_obj.keys();
                        bool is_view = false;
                        if (std::find(table_keys.begin(), table_keys.end(), "is_view") != table_keys.end()) {
                            // Convert to rvalue to access boolean value (or integer 0/1)
                            auto table_json_str = table_obj.dump();
                            auto table_rvalue = crow::json::load(table_json_str);
                            if (table_rvalue && table_rvalue.has("is_view")) {
                                auto& val = table_rvalue["is_view"];
                                if (val.t() == crow::json::type::True) {
                                    is_view = true;
                                } else if (val.t() == crow::json::type::Number) {
                                    is_view = (val.i() != 0);
                                }
                            }
                        }
                        table_entry["type"] = is_view ? "view" : "table";
                        table_entry["qualified_name"] = schema_name + "." + table_name;
                        tables_list[tables_list.size()] = std::move(table_entry);
                        
                        // Add column entries
                        if (std::find(table_keys.begin(), table_keys.end(), "columns") != table_keys.end()) {
                            for (const auto& column_name : table_obj["columns"].keys()) {
                                const auto& column_obj = table_obj["columns"][column_name];
                                
                                crow::json::wvalue column_entry;
                                column_entry["name"] = column_name;
                                column_entry["table"] = table_name;
                                column_entry["schema"] = schema_name;
                                
                                // Extract column type and nullable info
                                auto column_json_str = column_obj.dump();
                                auto column_rvalue = crow::json::load(column_json_str);
                                std::string column_type = "UNKNOWN";
                                bool nullable = true;
                                
                                if (column_rvalue && column_rvalue.has("type")) {
                                    column_type = column_rvalue["type"].s();
                                }
                                if (column_rvalue && column_rvalue.has("nullable")) {
                                    auto& val = column_rvalue["nullable"];
                                    if (val.t() == crow::json::type::True) {
                                        nullable = true;
                                    } else if (val.t() == crow::json::type::False) {
                                        nullable = false;
                                    } else if (val.t() == crow::json::type::Number) {
                                        nullable = (val.i() != 0);
                                    }
                                }
                                
                                column_entry["type"] = column_type;
                                column_entry["nullable"] = nullable;
                                column_entry["qualified_name"] = schema_name + "." + table_name + "." + column_name;
                                columns_list[columns_list.size()] = std::move(column_entry);
                            }
                        }
                    }
                }
            }
            
            completion_response["tables"] = std::move(tables_list);
            completion_response["columns"] = std::move(columns_list);
            return crow::response(200, completion_response);
        }

        return crow::response(200, response);
    } catch (const std::exception& e) {
        CROW_LOG_ERROR << "========================================";
        CROW_LOG_ERROR << "SCHEMA HANDLER EXCEPTION";
        CROW_LOG_ERROR << "========================================";
        CROW_LOG_ERROR << "Error type: " << typeid(e).name();
        CROW_LOG_ERROR << "Error message: " << e.what();
        CROW_LOG_ERROR << "Exception caught in SchemaHandler::getSchema";
        CROW_LOG_ERROR << "========================================";
        return crow::response(500, std::string("Internal server error: ") + e.what());
    } catch (...) {
        CROW_LOG_ERROR << "========================================";
        CROW_LOG_ERROR << "SCHEMA HANDLER UNKNOWN EXCEPTION";
        CROW_LOG_ERROR << "========================================";
        CROW_LOG_ERROR << "Unknown exception type caught in SchemaHandler::getSchema";
        CROW_LOG_ERROR << "========================================";
        return crow::response(500, "Internal server error: Unknown exception in schema handler");
    }
}

crow::response SchemaHandler::refreshSchema(const crow::request& req) {
    try {
        CROW_LOG_INFO << "Schema refresh requested";
        
        // Get database manager instance
        auto db_manager = DatabaseManager::getInstance();
        if (!db_manager) {
            CROW_LOG_ERROR << "DatabaseManager instance is null during schema refresh";
            return crow::response(500, "Internal server error: Database manager not initialized");
        }

        // Re-initialize database connections from config
        CROW_LOG_INFO << "Re-initializing database connections for schema refresh";
        db_manager->initializeDBManagerFromConfig(config_manager_);
        CROW_LOG_INFO << "Schema refresh completed successfully";

        return crow::response(200);
    } catch (const std::exception& e) {
        CROW_LOG_ERROR << "========================================";
        CROW_LOG_ERROR << "SCHEMA REFRESH FAILED";
        CROW_LOG_ERROR << "========================================";
        CROW_LOG_ERROR << "Error type: " << typeid(e).name();
        CROW_LOG_ERROR << "Error message: " << e.what();
        CROW_LOG_ERROR << "Exception caught in SchemaHandler::refreshSchema";
        CROW_LOG_ERROR << "========================================";
        return crow::response(500, std::string("Internal server error: ") + e.what());
    } catch (...) {
        CROW_LOG_ERROR << "========================================";
        CROW_LOG_ERROR << "SCHEMA REFRESH UNKNOWN EXCEPTION";
        CROW_LOG_ERROR << "========================================";
        CROW_LOG_ERROR << "Unknown exception type caught in SchemaHandler::refreshSchema";
        CROW_LOG_ERROR << "========================================";
        return crow::response(500, "Internal server error: Unknown exception in schema refresh");
    }
}




} // namespace flapi
