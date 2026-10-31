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
};

// `flapi cache refresh`: refresh the selected caches once, print a JSON summary, return the
// process exit code - 0 all refreshed, 1 at least one refresh failed, 2 usage error
// (nothing selected, unknown endpoint, read-only catalog). Opens no HTTP port, runs no warm-up.
int RunCacheRefreshCommand(std::shared_ptr<ConfigManager> config_manager,
                           const CacheRefreshOptions& options, std::ostream& out);

} // namespace flapi
