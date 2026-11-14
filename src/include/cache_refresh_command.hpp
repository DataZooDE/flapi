#pragma once

#include <memory>
#include <ostream>
#include <string>
#include <vector>

namespace flapi {

class ConfigManager;

struct CacheRefreshOptions {
    std::vector<std::string> endpoints;  // url paths
    bool all = false;
    bool full = false;  // force a full rebuild (watermark erased, table rebuilt)
};

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
