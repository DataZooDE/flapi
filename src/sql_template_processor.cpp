#include "sql_template_processor.hpp"
#include "flapi_tracing.hpp"
#include "trace_semconv.hpp"
#include "vfs_adapter.hpp"
#include "template_secrets.hpp"
#include "mustache_syntax_check.hpp"
#include "external_access_policy.hpp"
#include "auth_params.hpp"
#include <stdexcept>
#include <tuple>
#include <filesystem>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <sys/stat.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace flapi {

namespace {

// Local template files, cached by path and revalidated with ONE stat per read
// (#243). Every render used to re-read the file - two existence probes (four
// stats), an open, a read and two string copies - so an edited template was
// picked up by the next request. It still is: an entry is used only while the
// file's identity, size, mtime and ctime are what they were when it was read,
// and a rewrite changes ctime even when it restores the old mtime and size.
// Two writes within one timestamp tick of the filesystem can still look alike;
// every write flAPI makes itself (config service template PUT, endpoint
// create/update, reload) therefore drops the cache (clearTemplateCache). An
// external editor writing twice within one tick is the remaining gap.
struct FileStamp {
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
    std::int64_t size = -1;
    std::int64_t mtime_ns = 0;
    std::int64_t ctime_ns = 0;
    bool operator==(const FileStamp& o) const {
        return device == o.device && inode == o.inode && size == o.size && mtime_ns == o.mtime_ns &&
               ctime_ns == o.ctime_ns;
    }
};

std::optional<FileStamp> StampOf(const std::string& path) {
#ifdef _WIN32
    // Not _stat64: its times have whole seconds, its st_ctime is the CREATION
    // time and st_ino is 0, so two same-size edits within one second looked
    // alike. GetFileAttributesEx is still one call and reports the last write
    // in 100 ns units. There is no inode: delete-and-recreate is caught by the
    // creation time instead.
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!::GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data) ||
        (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return std::nullopt;
    }
    const auto ticks = [](const FILETIME& t) {
        return static_cast<std::int64_t>((static_cast<std::uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime);
    };
    FileStamp stamp;
    stamp.size = static_cast<std::int64_t>((static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow);
    stamp.mtime_ns = ticks(data.ftLastWriteTime) * 100;
    stamp.ctime_ns = ticks(data.ftCreationTime) * 100;
    return stamp;
#else
    struct stat st;
    if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        return std::nullopt;
    }
    FileStamp stamp;
    stamp.device = static_cast<std::uint64_t>(st.st_dev);
    stamp.inode = static_cast<std::uint64_t>(st.st_ino);
    stamp.size = static_cast<std::int64_t>(st.st_size);
#if defined(__APPLE__)
    stamp.mtime_ns = static_cast<std::int64_t>(st.st_mtimespec.tv_sec) * 1000000000LL + st.st_mtimespec.tv_nsec;
    stamp.ctime_ns = static_cast<std::int64_t>(st.st_ctimespec.tv_sec) * 1000000000LL + st.st_ctimespec.tv_nsec;
#else
    stamp.mtime_ns = static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
    stamp.ctime_ns = static_cast<std::int64_t>(st.st_ctim.tv_sec) * 1000000000LL + st.st_ctim.tv_nsec;
#endif
    return stamp;
#endif
}

struct CachedTemplate {
    FileStamp stamp;
    std::shared_ptr<const std::string> text;
};

// Keyed by the resolved path: the key space is the configured templates.
constexpr std::size_t kMaxCachedTemplates = 4096;

std::mutex& TemplateCacheMutex() {
    static std::mutex m;
    return m;
}

std::unordered_map<std::string, CachedTemplate>& TemplateCache() {
    static std::unordered_map<std::string, CachedTemplate> cache;
    return cache;
}

}  // namespace

void SQLTemplateProcessor::clearTemplateCache() {
    std::lock_guard<std::mutex> lock(TemplateCacheMutex());
    TemplateCache().clear();
}

void SQLTemplateProcessor::forEachAllowedEnvironmentVariable(
    const TemplateConfig& whitelist,
    const std::function<void(const std::string& name, std::string_view value)>& visit) {
    // An empty whitelist allows none: nothing to walk.
    if (whitelist.environment_whitelist.empty()) {
        return;
    }
    std::string key;
    for (char** env = environ; *env != nullptr; ++env) {
        const char* entry = *env;
        const char* eq = std::strchr(entry, '=');
        if (eq == nullptr) {
            continue;
        }
        key.assign(entry, eq - entry);
        if (whitelist.isEnvironmentVariableAllowed(key)) {
            visit(key, std::string_view(eq + 1));
        }
    }
}

SQLTemplateProcessor::SQLTemplateProcessor(std::shared_ptr<ConfigManager> config_manager)
    : config_manager(config_manager) {}

std::string SQLTemplateProcessor::loadAndProcessTemplate(const EndpointConfig& endpoint, std::map<std::string, std::string>& params) {
    std::string templatePath = getFullTemplatePath(endpoint.templateSource);
    CROW_LOG_DEBUG << "Template path: " << templatePath;
    std::string templateContent = loadTemplateContent(templatePath);
    RequireValidMustacheSyntax(templateContent, templatePath);
    AuthRender auth;
    crow::mustache::context ctx = createTemplateContext(endpoint, params, &auth);
    return processTemplate(templateContent, ctx, auth, templatePath);
}

PreparedQueryRender SQLTemplateProcessor::loadAndProcessTemplatePrepared(
    const EndpointConfig& endpoint, std::map<std::string, std::string>& params) {
    PreparedQueryRender out;

    // No context parameter is threaded here, deliberately. OpenTelemetry keeps a
    // thread-local active-span stack and the middleware's SpanScope holds the
    // corresponding Scope, so this child attaches to the server span by itself.
    // Adding a parameter would have touched every caller - the cache manager, the
    // heartbeat worker, mcp_dry_run, the config service warmup - and every
    // existing test, for no behavioural gain.
    SpanScope span = Tracing().startSpan("flapi.render_template", SpanKind::Internal);

    const std::string templatePath = getFullTemplatePath(endpoint.templateSource);
    CROW_LOG_DEBUG << "Prepared template path: " << templatePath;
    const auto templateText = loadTemplateShared(templatePath);
    const std::string& templateContent = *templateText;
    // Checked on the SOURCE, so an error names the file and a line the
    // operator can find; the rewritten text is checked again at compile.
    RequireValidMustacheSyntax(templateContent, templatePath);

    PreparedTemplateRewriter rewriter;
    SqlParameterClassifier classifier;
    auto rewrite = rewriter.rewrite(templateContent, endpoint.request_fields, classifier);

    // createTemplateContext mutates params (it strips cache/auth keys
    // before exposing the remainder as params.*). Run it after rewriting
    // so the rewriter still sees the auth-prefixed keys it ignores by
    // design (they are not declared as request_fields, so the rewriter's
    // findField() path returns nullptr and they pass through untouched).
    AuthRender auth;
    crow::mustache::context ctx = createTemplateContext(endpoint, params, &auth);

    out.sql = processTemplate(rewrite.rewritten_template, ctx, auth, templatePath);
    out.bindings = std::move(rewrite.bindings);

    if (span) {
        // Config-relative, never absolute. endpoint.templateSource is resolved to
        // an absolute path at config-load time, and an absolute path discloses the
        // server's filesystem layout while telling a trace consumer nothing they
        // can use. TELEMETRY.md already excludes file paths for the same reason.
        std::string_view relative(endpoint.templateSource);
        if (const auto slash = relative.find_last_of('/'); slash != std::string_view::npos) {
            relative.remove_prefix(slash + 1);
        }
        span.setAttr(semconv::flapix::kTemplatePath, relative);
        span.setAttr(semconv::flapix::kTemplateBytes,
                     static_cast<std::int64_t>(templateContent.size()));
        // The prepared-vs-interpolated split is a SECURITY-relevant signal and
        // uniquely flAPI's to report: an interpolated site is one where a value is
        // substituted into SQL text rather than bound as a parameter.
        span.setAttr(semconv::flapix::kPreparedCount,
                     static_cast<std::int64_t>(out.bindings.size()));
    }
    return out;
}

PreparedQueryRender SQLTemplateProcessor::processTemplatePrepared(const std::string& templateContent,
                                                                  const EndpointConfig& endpoint,
                                                                  std::map<std::string, std::string>& params) {
    RequireValidMustacheSyntax(templateContent, endpoint.templateSource);
    PreparedTemplateRewriter rewriter;
    SqlParameterClassifier classifier;
    auto rewrite = rewriter.rewrite(templateContent, endpoint.request_fields, classifier);
    AuthRender auth;
    crow::mustache::context ctx = createTemplateContext(endpoint, params, &auth);
    PreparedQueryRender out;
    out.sql = processTemplate(rewrite.rewritten_template, ctx, auth, endpoint.templateSource);
    out.bindings = std::move(rewrite.bindings);
    return out;
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
    RequireValidMustacheSyntax(templateContent, templatePath);
    AuthRender auth;
    crow::mustache::context ctx = createTemplateContext(endpoint, params, &auth);
    return processTemplate(templateContent, ctx, auth, templatePath);
}

std::string SQLTemplateProcessor::processTemplate(const std::string& templateContent, const crow::mustache::context& ctx) {
    return processTemplate(templateContent, ctx, AuthRender{}, "");
}

std::string SQLTemplateProcessor::processTemplate(const std::string& templateContent,
                                                  const crow::mustache::context& ctx,
                                                  const AuthRender& auth,
                                                  const std::string& label) {
    auto compiledTemplate = compileTemplate(templateContent);
    auto renderedTemplate = compiledTemplate.render(ctx).dump();
    if (!auth.markers.empty()) {
        // Every auth.* value must land in a plain top-level '...' literal of the
        // RENDERED SQL (see createTemplateContext); only then is it substituted.
        const auto problems = FindUnsafeAuthMarkers(renderedTemplate, auth.prefix, auth.markers);
        if (!problems.empty()) {
            const std::string where = "the rendered SQL of " + (label.empty() ? std::string("the template") : label);
            throw std::invalid_argument(FormatAuthPlacementProblem(problems.front(), where));
        }
        renderedTemplate = SubstituteAuthMarkers(renderedTemplate, auth.prefix, auth.markers);
    }
    if (crow::logger::get_current_log_level() <= crow::LogLevel::Debug) {
        // The rendered SQL carries whatever the template interpolated, a
        // connection password included. Every connection's credential-valued
        // properties are scrubbed, whichever endpoint this renders for.
        TemplateSecrets secrets;
        if (config_manager) {
            for (const auto& [name, connection] : config_manager->getConnections()) {
                secrets.addAll(connection.properties, TemplateSecrets::Source::Connection);
            }
        }
        CROW_LOG_DEBUG << "Processed query: \n" << secrets.scrub(renderedTemplate);
    }
    // duckdb-hardening (#253): every template - endpoint, cache populate, MCP tool,
    // config-service expand - is rendered here, so this is the one place INSTALL/LOAD
    // and secret DDL are refused before DuckDB sees them.
    RefuseRestrictedTemplateStatements(renderedTemplate);
    return renderedTemplate;
}

crow::mustache::template_t SQLTemplateProcessor::compileTemplate(const std::string& templateSource) {
    // The one road into crow::mustache::compile. Templates are revalidated
    // against the file on every render, so a file edited after load (without
    // a reload) reaches this point unchecked; crow itself would segfault or hang on an
    // unbalanced section (see mustache_syntax_check.hpp).
    RequireValidMustacheSyntax(templateSource, "");
    return crow::mustache::compile(templateSource);
}

std::string SQLTemplateProcessor::loadTemplateContent(const std::string& templatePath) {
    return *loadTemplateShared(templatePath);
}

std::shared_ptr<const std::string> SQLTemplateProcessor::loadTemplateShared(const std::string& templatePath) {
    auto file_provider = config_manager->getFileProvider();
    // Only plain local files are cached: a bundle is served from memory
    // already, and a remote template is re-fetched per read, as before.
    if (!PathSchemeUtils::IsRemotePath(templatePath) && file_provider->GetProviderName() == "local") {
        const std::string local = PathSchemeUtils::StripFileScheme(templatePath);
        const auto stamp = StampOf(local);
        if (!stamp) {
            {
                std::lock_guard<std::mutex> lock(TemplateCacheMutex());
                TemplateCache().erase(local);
            }
            throw std::runtime_error("Template file not found: " + templatePath);
        }
        {
            std::lock_guard<std::mutex> lock(TemplateCacheMutex());
            const auto it = TemplateCache().find(local);
            if (it != TemplateCache().end() && it->second.stamp == *stamp) {
                return it->second.text;
            }
        }
        // Read AFTER the stamp: a write racing this read leaves a newer stamp
        // on disk than the one stored, so the next read re-reads.
        auto text = std::make_shared<const std::string>(file_provider->ReadFile(templatePath));
        std::lock_guard<std::mutex> lock(TemplateCacheMutex());
        auto& cache = TemplateCache();
        if (cache.size() >= kMaxCachedTemplates && cache.find(local) == cache.end()) {
            cache.clear();
        }
        cache[local] = CachedTemplate{*stamp, text};
        return text;
    }
    return std::make_shared<const std::string>(readTemplateUncached(*file_provider, templatePath));
}

std::string SQLTemplateProcessor::readTemplateUncached(IFileProvider& provider, const std::string& templatePath) {
    auto* file_provider = &provider;
    if (PathSchemeUtils::IsRemotePath(templatePath)) {
        // No separate existence probe: it turned a 403, an expired token or a dead store into
        // "not found" and cost a round trip. ReadFile says what went wrong (status, redacted URL).
        try {
            return file_provider->ReadFile(templatePath);
        } catch (const FileOperationError& e) {
            throw std::runtime_error("Could not read template " + PathSchemeUtils::RedactUrl(templatePath) + ": " + e.what());
        }
    }
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
        return PathSchemeUtils::JoinUrl(basePath, templateSource);
    }

    // In bundled mode the endpoint parser already resolved templateSource
    // against the YAML's parent directory, so it carries the full bundle
    // key (e.g. "sqls/hello.sql"). Joining with the basePath ("sqls")
    // would produce "sqls/sqls/hello.sql". Mirror the absolute-path
    // short-circuit above: if templateSource already starts with the
    // basePath prefix, return it as-is.
    {
        std::string base_with_sep = basePath;
        if (!base_with_sep.empty() && base_with_sep.back() != '/') {
            base_with_sep += '/';
        }
        if (!base_with_sep.empty() &&
            templateSource.compare(0, base_with_sep.size(), base_with_sep) == 0) {
            return templateSource;
        }
    }

    // Local path resolution
    std::filesystem::path fullPath = std::filesystem::path(basePath) / templateSource;
    return fullPath.string();
}

crow::mustache::context SQLTemplateProcessor::createTemplateContext(const EndpointConfig& endpoint, std::map<std::string, std::string>& params) {
    return createTemplateContext(endpoint, params, nullptr);
}

crow::mustache::context SQLTemplateProcessor::createTemplateContext(const EndpointConfig& endpoint,
                                                                    std::map<std::string, std::string>& params,
                                                                    AuthRender* auth) {
    crow::mustache::context ctx;

    // Add connection properties
    if (!endpoint.connection.empty()) {
        for (const auto& [key, value] : config_manager->getPropertiesForTemplates(endpoint.connection[0])) {
            ctx["conn"][key] = value;
        }
    }

    // Add filtered environment variables
    // Straight from `environ`, only the whitelisted names: copying the whole environment into a
    // map on every request was ~11% of a small query's CPU. An empty whitelist allows none.
    forEachAllowedEnvironmentVariable(config_manager->getTemplateConfig(),
                                      [&](const std::string& name, std::string_view value) {
                                          ctx["env"][name] = std::string(value);
                                      });

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

    // Extract auth params (reserved __auth_ prefix) and inject into ctx["auth"].
    //
    // These values come from the identity provider (a Basic username, JWT
    // `sub`/`roles`, OIDC claims, some of which end users can edit), and they
    // are never bound as prepared parameters: the rewriter only binds params.*.
    // They are made safe for exactly one place, a plain top-level '...'
    // literal of the outer query, by doubling every single quote (DuckDB gives
    // backslash no meaning there, and O'Brien still matches 'O''Brien').
    //
    // So the template is rendered with a random MARKER in place of each
    // non-empty value (an empty one renders empty, so sections behave alike),
    // the rendered SQL is lexed, every marker must sit in such a literal, and
    // only then are the quote-doubled values substituted (processTemplate).
    // Checking the rendered SQL rather than the source means no Mustache
    // comment or section between two SQL characters can hide where a value
    // lands. Without `auth` (the public overload) the values render empty.
    static const std::vector<std::tuple<std::string, std::string, char>> auth_keys = {
        {"__auth_username",      "username", 'a'},
        {"__auth_roles",         "roles",    'b'},
        {"__auth_email",         "email",    'c'},
        {"__auth_type",          "type",     'd'},
        {"__auth_groups",        "groups",   'e'},
        {"__auth_authenticated", "authenticated", '\0'},
    };
    if (auth != nullptr) {
        auth->prefix = NewAuthMarkerPrefix();
        auth->markers.clear();
    }
    for (const auto& [param_key, ctx_key, letter] : auth_keys) {
        auto it = params.find(param_key);
        if (it == params.end()) {
            continue;
        }
        // A control character (newline, carriage return, NUL, ...) is
        // refused, not stripped. Stripping would turn "bob\n" into "bob", a
        // different principal's identity, and a newline or NUL is no part of
        // a real name. The authentication layers (AuthMiddleware,
        // MCPAuthHandler) already answer 401 for such an identity; this is the
        // backstop for any path that reaches a render without them.
        if (ContainsControlCharacter(it->second)) {
            throw std::invalid_argument("auth." + ctx_key + " contains a control character; refusing to render it");
        }
        if (letter == '\0') {
            ctx["auth"][ctx_key] = it->second;   // "true", set by the server
        } else if (auth == nullptr || it->second.empty()) {
            ctx["auth"][ctx_key] = std::string();
        } else {
            std::string quoted;
            quoted.reserve(it->second.size());
            for (const char c : it->second) {
                quoted += c;
                if (c == '\'') {
                    quoted += '\'';
                }
            }
            std::string marker = auth->prefix + letter + 'z';
            ctx["auth"][ctx_key] = marker;
            auth->markers.push_back(AuthMarker{std::move(marker), "auth." + ctx_key, std::move(quoted)});
        }
        params.erase(it);
    }

    // Add request parameters
    for (const auto& [key, value] : params) {
        ctx["params"][key] = value;
    }

    // Deliberately NOT dumped: the context carries `conn.*`, i.e. connection
    // passwords, tokens and DSNs, and a debug-level log is routinely shipped to
    // a log aggregator. Only the shape is logged.
    CROW_LOG_DEBUG << "Template context built: " << params.size() << " request parameter(s)";

    return ctx;
}

std::string SQLTemplateProcessor::loadTemplate(const EndpointConfig& endpoint) {
    std::string templatePath = getFullTemplatePath(endpoint.templateSource);
    return loadTemplateContent(templatePath);
}

} // namespace flapi