#include "cache_refresh_command.hpp"
#include "credential_manager.hpp"
#include "vfs_adapter.hpp"

#include <crow.h>
#include <algorithm>
#include <cctype>
#include <filesystem>

#include "cache_manager.hpp"
#include "config_manager.hpp"
#include "database_manager.hpp"
#include "duckdb_embed_fs.hpp"

namespace flapi {

namespace {

int usageError(std::ostream& out, const std::string& message) {
    crow::json::wvalue body;
    body["error"] = message;
    out << body.dump() << std::endl;
    return 2;
}

} // namespace

bool IsCatalogLockError(const std::string& message) {
    // DuckDB's wording when another process holds the file lock on the DuckLake
    // metadata database: "Could not set lock on file ...: Conflicting lock is held
    // in ... (PID n)".
    return message.find("Could not set lock on file") != std::string::npos ||
           message.find("Conflicting lock is held") != std::string::npos;
}

std::string CatalogLockMessage(const std::string& endpoint_path) {
    // The holder may be read-only replicas, which cannot refresh at all - so
    // "refresh through the server" alone was a dead end there.
    return "a running flapi holds the DuckLake catalog lock; refresh through the server: flapii cache refresh " +
           (endpoint_path.empty() ? std::string("<path>") : endpoint_path) +
           " (if the holders are read-only replicas, stop the replicas, or use a Postgres/SQLite/MySQL "
           "DuckLake catalog, which a writer and replicas can share)";
}

CachedEndpointLookup FindCachedEndpointForPath(const ConfigManager& config_manager, const std::string& path,
                                               const std::string& method) {
    CachedEndpointLookup out;
    std::string wanted_method = method;
    std::transform(wanted_method.begin(), wanted_method.end(), wanted_method.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    const auto snapshot = config_manager.getEndpoints();
    std::vector<const EndpointConfig*> on_path;
    for (const auto& ep : *snapshot) {
        if (ep.urlPath != path) {
            continue;
        }
        std::string ep_method = ep.method.empty() ? std::string("GET") : ep.method;
        std::transform(ep_method.begin(), ep_method.end(), ep_method.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        if (!wanted_method.empty() && ep_method != wanted_method) {
            continue;
        }
        on_path.push_back(&ep);
    }
    if (on_path.empty()) {
        // A templated path (/orders/:id) named by a concrete one.
        const auto* matched = wanted_method.empty() ? ConfigManager::findEndpoint(*snapshot, path)
                                                    : ConfigManager::findEndpoint(*snapshot, path, wanted_method);
        if (matched) {
            on_path.push_back(matched);
        }
    }
    if (on_path.empty()) {
        out.error = "unknown endpoint " + path + (wanted_method.empty() ? "" : " with method " + wanted_method);
        return out;
    }
    out.found = true;
    // Only endpoints WITH a cache count: an uncached POST /orders beside the
    // cached GET /orders used to be picked (first in load order) and the
    // refresh failed with "has no cache".
    std::vector<const EndpointConfig*> cached;
    for (const auto* ep : on_path) {
        if (ep->cache.enabled && !ep->cache.table.empty()) {
            cached.push_back(ep);
        }
    }
    if (cached.empty()) {
        out.error = "endpoint " + path + (wanted_method.empty() ? "" : " (" + wanted_method + ")") + " has no cache";
        return out;
    }
    const auto* chosen = cached.front();
    for (const auto* ep : cached) {
        if (ep->cache.table != chosen->cache.table || ep->cache.schema != chosen->cache.schema) {
            std::string methods;
            for (const auto* c : cached) {
                methods += (methods.empty() ? "" : "|") + (c->method.empty() ? std::string("GET") : c->method);
            }
            out.error = "ambiguous: several endpoints on " + path + " cache into different tables; pass --method " +
                        methods;
            return out;
        }
        if (!chosen->cache.hasTemplate() && ep->cache.hasTemplate()) {
            chosen = ep;   // the one naming a populate template, as warm-up picks
        }
    }
    out.endpoint = *chosen;
    return out;
}

int RunCacheRefreshCommand(std::shared_ptr<ConfigManager> config_manager,
                           const CacheRefreshOptions& options, std::ostream& out) {
    const auto& ducklake = config_manager->getDuckLakeConfig();
    if (!ducklake.enabled) {
        return usageError(out, "ducklake is not enabled in this configuration");
    }
    if (ducklake.read_only) {
        return usageError(out, "the DuckLake catalog is configured read-only (ducklake.read-only); "
                               "run the refresh with the writer's configuration");
    }

    // Copies: the refresh outlives no snapshot, and a copy cannot dangle.
    std::vector<EndpointConfig> selected;
    if (options.all) {
        // One refresh per cache table, as warm-up does: endpoints sharing a table
        // would otherwise rebuild it once each.
        selected = CacheManager::warmupEndpoints(config_manager);
    }
    for (const auto& path : options.endpoints) {
        const auto lookup = FindCachedEndpointForPath(*config_manager, path, options.method);
        if (!lookup.endpoint) {
            return usageError(out, lookup.error);
        }
        selected.push_back(*lookup.endpoint);
    }
    if (selected.empty()) {
        return usageError(out, "nothing to refresh: pass --endpoint <path> or --all");
    }
    const std::string hint_path = options.endpoints.empty() ? std::string() : options.endpoints.front();

    auto db_manager = DatabaseManager::getInstance();
    try {
        if (!db_manager->isInitialized()) {
            db_manager->initializeDBManagerFromConfig(config_manager);
        }
    } catch (const std::exception& ex) {
        // The raw DuckDB lock text names a file and a PID but not what to do.
        if (IsCatalogLockError(ex.what())) {
            crow::json::wvalue body;
            body["error"] = CatalogLockMessage(hint_path);
            out << body.dump() << std::endl;
            return 1;
        }
        throw;
    }
    // embed:// for the populate SQL: the bundle's when packed, else the config
    // directory. The one-shot never registered it, so a packed binary's refresh
    // of a read_csv('embed://...') cache failed too.
    {
        const std::string base = config_manager->getBasePath();
        RegisterEmbeddedFileSystem(
            std::filesystem::absolute(base.empty() || PathSchemeUtils::IsRemotePath(base) ? std::string(".") : base).string());
    }
    auto cache_manager = db_manager->getCacheManager();
    cache_manager->initializeAuditTables(config_manager);

    // The one-shot runs the same populate templates as the server, so it gets the same
    // cloud credentials (a remote config's own bucket, AWS_ENDPOINT_URL) and the same
    // duckdb-hardening policy (#253), last, as the server does.
    {
        auto& credentials = getGlobalCredentialManager();
        credentials.loadFromEnvironment();
        credentials.configureDuckDB();
    }
    db_manager->applyExternalAccessPolicy();

    CacheManager::RefreshOptions refresh_options;
    refresh_options.full = options.full;

    int failed = 0;
    bool lock_error = false;
    std::vector<crow::json::wvalue> results;
    for (const auto& endpoint : selected) {
        crow::json::wvalue item;
        item["endpoint"] = endpoint.urlPath;
        item["schema"] = endpoint.cache.schema;
        item["table"] = endpoint.cache.table;
        if (options.full) {
            item["full"] = true;
        }
        std::map<std::string, std::string> params;
        try {
            cache_manager->refreshCache(config_manager, endpoint, params, refresh_options);
            item["status"] = "ok";
        } catch (const std::exception& ex) {
            ++failed;
            item["status"] = "failed";
            if (IsCatalogLockError(ex.what())) {
                lock_error = true;
                item["error"] = CatalogLockMessage(endpoint.urlPath);
            } else {
                item["error"] = std::string(ex.what());
            }
        }
        // What the refresh measured - the same numbers the audit log records.
        for (const auto& stats : cache_manager->getRefreshStats()) {
            const std::string schema = endpoint.cache.schema.empty() ? "main" : endpoint.cache.schema;
            if (stats.schema != schema || stats.table != endpoint.cache.table) {
                continue;
            }
            if (stats.last_duration_ms) {
                item["duration_ms"] = *stats.last_duration_ms;
            }
            if (stats.last_rows_affected) {
                item["rows_affected"] = *stats.last_rows_affected;
            } else {
                item["rows_affected"] = nullptr;
            }
            break;
        }
        results.push_back(std::move(item));
    }

    // The one-shot is how cron/k8s deployments without a long-running writer refresh,
    // so it is also where their compaction has to happen. Due-ness comes from the
    // audit log, so a frequent cron does not compact on every run.
    if (!lock_error) {
        try {
            if (cache_manager->compactionDue(config_manager, std::chrono::system_clock::now())) {
                cache_manager->performCompaction(config_manager);
            }
        } catch (const std::exception& ex) {
            CROW_LOG_WARNING << "DuckLake compaction failed: " << ex.what();
        }
    }

    crow::json::wvalue summary;
    summary["refreshed"] = std::move(results);
    summary["failed"] = failed;
    if (lock_error) {
        summary["error"] = CatalogLockMessage(hint_path);
    }
    out << summary.dump() << std::endl;
    return failed > 0 ? 1 : 0;
}

} // namespace flapi
