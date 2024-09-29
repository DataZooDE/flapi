#pragma once

#include <string>
#include <chrono>
#include "config_manager.hpp"
#include <duckdb.h>

namespace flapi {

class DatabaseManager; // Forward declaration

class CacheManager {
public:
    CacheManager(std::shared_ptr<DatabaseManager> db_manager);
    
    std::string getCacheTableName(const CacheConfig& cacheConfig);

    void warmUpCaches(std::shared_ptr<ConfigManager> config_manager);
    bool shouldRefreshCache(std::shared_ptr<ConfigManager> config_manager, const EndpointConfig& endpoint);
    bool shouldRefreshCache(std::shared_ptr<ConfigManager> config_manager, const CacheConfig& cacheConfig);

    void refreshCache(std::shared_ptr<ConfigManager> config_manager, const EndpointConfig& endpoint, std::map<std::string, std::string>& params);
    void addQueryCacheParamsIfNecessary(std::shared_ptr<ConfigManager> config_manager, const EndpointConfig& endpoint, std::map<std::string, std::string>& params);
private:
    std::shared_ptr<DatabaseManager> db_manager;
    std::string createCacheTableName(const std::string& baseName);
    std::chrono::system_clock::time_point getTableCreationTime(const std::string& schema, const std::string& table);
};

} // namespace flapi