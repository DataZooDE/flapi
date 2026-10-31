#include "cache_refresh_command.hpp"

#include <crow.h>

#include "cache_manager.hpp"
#include "config_manager.hpp"
#include "database_manager.hpp"

namespace flapi {

namespace {

int usageError(std::ostream& out, const std::string& message) {
    crow::json::wvalue body;
    body["error"] = message;
    out << body.dump() << std::endl;
    return 2;
}

} // namespace

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
        for (const auto& endpoint : *config_manager->getEndpoints()) {
            if (endpoint.cache.enabled && !endpoint.cache.table.empty()) {
                selected.push_back(endpoint);
            }
        }
    }
    for (const auto& path : options.endpoints) {
        auto endpoint = config_manager->getEndpointForPath(path);
        if (!endpoint) {
            return usageError(out, "unknown endpoint " + path);
        }
        if (!endpoint->cache.enabled || endpoint->cache.table.empty()) {
            return usageError(out, "endpoint " + path + " has no cache");
        }
        selected.push_back(*endpoint);
    }
    if (selected.empty()) {
        return usageError(out, "nothing to refresh: pass --endpoint <path> or --all");
    }

    auto db_manager = DatabaseManager::getInstance();
    if (!db_manager->isInitialized()) {
        db_manager->initializeDBManagerFromConfig(config_manager);
    }
    auto cache_manager = db_manager->getCacheManager();
    cache_manager->initializeAuditTables(config_manager);

    int failed = 0;
    std::vector<crow::json::wvalue> results;
    for (const auto& endpoint : selected) {
        crow::json::wvalue item;
        item["endpoint"] = endpoint.urlPath;
        item["schema"] = endpoint.cache.schema;
        item["table"] = endpoint.cache.table;
        std::map<std::string, std::string> params;
        try {
            cache_manager->refreshCache(config_manager, endpoint, params);
            item["status"] = "ok";
        } catch (const std::exception& ex) {
            ++failed;
            item["status"] = "failed";
            item["error"] = std::string(ex.what());
        }
        results.push_back(std::move(item));
    }

    crow::json::wvalue summary;
    summary["refreshed"] = std::move(results);
    summary["failed"] = failed;
    out << summary.dump() << std::endl;
    return failed > 0 ? 1 : 0;
}

} // namespace flapi
