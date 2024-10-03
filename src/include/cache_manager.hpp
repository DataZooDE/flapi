#pragma once

#include <string>
#include <chrono>
#include "config_manager.hpp"
#include <duckdb.h>

namespace flapi {

class DatabaseManager; // Forward declaration

template<typename TWatermark>
struct CacheWatermark {
    std::string tableName;
    TWatermark watermark;

    std::string toTotalTableName() const {
        return tableName + "_" + std::to_string(watermark);
    }

    static CacheWatermark<int64_t> now(const std::string& baseName) {
        auto now = std::chrono::system_clock::now();
        auto epoch = now.time_since_epoch();
        auto seconds = std::chrono::duration_cast<std::chrono::seconds>(epoch);
        int64_t timestamp = seconds.count();
        std::string tableName = baseName + "_" + std::to_string(timestamp);
        return {tableName, timestamp};
    } 

    static CacheWatermark<int64_t> parseFromTableName(const std::string& tableName) 
    {
        size_t pos = tableName.rfind('_');
        if (pos == std::string::npos) {
            throw std::runtime_error("Invalid cache table name format: " + tableName);
        }
        std::string baseName = tableName.substr(0, pos);
        int64_t timestamp = std::stoll(tableName.substr(pos + 1));
        return CacheWatermark<int64_t>{baseName, timestamp};
    }
};

class CacheManager {
public:
    CacheManager(std::shared_ptr<DatabaseManager> db_manager);

    void warmUpCaches(std::shared_ptr<ConfigManager> config_manager);
    bool shouldRefreshCache(std::shared_ptr<ConfigManager> config_manager, const EndpointConfig& endpoint);
    bool shouldRefreshCache(std::shared_ptr<ConfigManager> config_manager, const CacheConfig& cacheConfig);

    void refreshCache(std::shared_ptr<ConfigManager> config_manager, const EndpointConfig& endpoint, std::map<std::string, std::string>& params);
    void addQueryCacheParamsIfNecessary(std::shared_ptr<ConfigManager> config_manager, const EndpointConfig& endpoint, std::map<std::string, std::string>& params);
private:
    void addPreviousCacheTableParamsIfNecessary(const std::string& cacheTableName, std::map<std::string, std::string>& params);

    std::shared_ptr<DatabaseManager> db_manager;
};

} // namespace flapi