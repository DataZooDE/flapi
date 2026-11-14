#pragma once

#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

#include "config_manager.hpp"

namespace flapi {


struct CacheRefreshOptions {
    std::vector<std::string> endpoints;  // url paths
    bool all = false;
    bool full = false;  // force a full rebuild (watermark erased, table rebuilt)
    std::string method;  // --method: which of several endpoints on a path; "" = the cached one
};

// The cached endpoint `path` names for a cache operation. Several endpoints may
// share a url-path (a cached GET /orders beside an uncached POST /orders): only
// those WITH a cache count, and several on one table are one cache. `method`
// ("" for any) narrows it. Empty `error` on success; otherwise the message and
// `found` = whether any endpoint has that path.
struct CachedEndpointLookup {
    std::optional<EndpointConfig> endpoint;
    std::string error;
    bool found = false;
};
CachedEndpointLookup FindCachedEndpointForPath(const ConfigManager& config_manager, const std::string& path,
                                               const std::string& method);

// The DuckDB error a second process gets when a running server holds the DuckLake
// catalog's file lock, and the message the one-shot prints instead of it.
bool IsCatalogLockError(const std::string& message);
std::string CatalogLockMessage(const std::string& endpoint_path);

// `flapi cache refresh`: refresh the selected caches once, print a JSON summary, return the
// process exit code - 0 all refreshed, 1 at least one refresh failed (or the catalog is locked
// by a running server), 2 usage error
// (nothing selected, unknown endpoint, read-only catalog). Opens no HTTP port, runs no warm-up.
int RunCacheRefreshCommand(std::shared_ptr<ConfigManager> config_manager,
                           const CacheRefreshOptions& options, std::ostream& out);

} // namespace flapi
