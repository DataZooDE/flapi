#include <sstream>
#include <fstream>
#include <stdexcept>
#include <crow.h>
#include <regex>

#include "cache_manager.hpp"
#include "database_manager.hpp"

namespace flapi {

CacheManager::CacheManager(std::shared_ptr<DatabaseManager> db_manager) : db_manager(db_manager) {}

void CacheManager::warmUpCaches(std::shared_ptr<ConfigManager> config_manager) {
    CROW_LOG_INFO << "Warming up endpoint caches, this might take some time...";
    const auto& cache_schema = config_manager->getCacheSchema();

    std::map<std::string, std::string> params;
    for (auto &endpoint : config_manager->getEndpoints()) 
    {
        if (shouldRefreshCache(config_manager, endpoint)) 
        {
            refreshCache(config_manager, endpoint, params);
        }
    }
    CROW_LOG_INFO << "Finished warming up endpoint caches! Let's go!";
}

bool CacheManager::shouldRefreshCache(std::shared_ptr<ConfigManager> config_manager, const EndpointConfig& endpoint) {
    if ((!endpoint.cache.cacheTableName.empty()) && (!endpoint.cache.cacheSource.empty())) {
        CROW_LOG_INFO << "Checking if cache should be refreshed for endpoint: " << endpoint.urlPath;
        return shouldRefreshCache(config_manager, endpoint.cache);
    }
    return false;
}

bool CacheManager::shouldRefreshCache(std::shared_ptr<ConfigManager> config_manager, const CacheConfig& cacheConfig) {
    std::string cacheTableName = cacheConfig.cacheTableName;
    
    std::vector<std::string> tableNames = db_manager->getTableNames(config_manager->getCacheSchema(), cacheTableName, true);
    if (tableNames.empty()) {
        CROW_LOG_INFO << "Cache table not found: '" << cacheTableName << "%', need to refresh";
        return true;
    }

    if (cacheConfig.refreshTime.empty()) {
        return false;
    }

    auto newestCacheTable = tableNames.front();

    auto watermark = CacheWatermark<int64_t>::parseFromTableName(newestCacheTable);
    auto creationTime = std::chrono::system_clock::time_point(std::chrono::seconds(watermark.watermark));
    auto now = std::chrono::system_clock::now();
    auto tableAge = std::chrono::duration_cast<std::chrono::seconds>(now - creationTime);

    auto refreshSeconds = cacheConfig.getRefreshTimeInSeconds();
    auto newestTooOld = tableAge > refreshSeconds;
    if (newestTooOld) {
        CROW_LOG_INFO << "Cache too old: " << newestCacheTable << ": (table age) " << tableAge.count() << "s > " << refreshSeconds.count() << "s (refreshTime from config), need to refresh";
    }
    else {
        CROW_LOG_INFO << "Cache is fresh: " << newestCacheTable << ": (table age) " << tableAge.count() << "s <= " << refreshSeconds.count() << "s (refreshTime from config)";
    }
    return newestTooOld;
}

void CacheManager::refreshCache(std::shared_ptr<ConfigManager> config_manager, const EndpointConfig& endpoint, std::map<std::string, std::string>& params) {
    auto &cacheConfig = endpoint.cache;
    auto currentWatermark = CacheWatermark<int64_t>::now(cacheConfig.cacheTableName);
    auto cacheSchema = config_manager->getCacheSchema();

    params.emplace("cacheSchema", cacheSchema);
    params.emplace("cacheTableName", currentWatermark.tableName);
    params.emplace("cacheRefreshTime", cacheConfig.refreshTime);
    params.emplace("currentWatermark", std::to_string(currentWatermark.watermark));

    std::vector<std::string> previousCacheTables = db_manager->getTableNames(cacheSchema, cacheConfig.cacheTableName, true);
    if (! previousCacheTables.empty()) {
        addPreviousCacheTableParamsIfNecessary(previousCacheTables.front(), params);
    }
    
    CROW_LOG_INFO << "Starting to refresh cache: " << cacheSchema << "." << currentWatermark.tableName;
    
    db_manager->createSchemaIfNecessary(cacheSchema);
    db_manager->executeCacheQuery(endpoint, cacheConfig, params);
    
    CROW_LOG_INFO << "Cache refreshed: " << config_manager->getCacheSchema() << "." << currentWatermark.tableName;

    performGarbageCollection(config_manager, endpoint, previousCacheTables);
}

void CacheManager::addQueryCacheParamsIfNecessary(std::shared_ptr<ConfigManager> config_manager, const EndpointConfig& endpoint, std::map<std::string, std::string>& params) {
    auto &cacheConfig = endpoint.cache;
    if (cacheConfig.cacheTableName.empty() || cacheConfig.cacheSource.empty()) {
        return;
    }

    auto cacheTableName = cacheConfig.cacheTableName;
    std::vector<std::string> tableNames = db_manager->getTableNames(config_manager->getCacheSchema(), cacheTableName, true);

    if (tableNames.empty()) {
        throw std::runtime_error("Cache table not found: '" + cacheTableName + "', this should not happen, cache should be created or refreshed before query.");
    }

    auto currentWatermark = CacheWatermark<int64_t>::parseFromTableName(tableNames.front());

    params.emplace("cacheSchema", config_manager->getCacheSchema());
    params.emplace("cacheTableName", tableNames.front());
    params.emplace("cacheRefreshTime", cacheConfig.refreshTime);
    params.emplace("currentWatermark", std::to_string(currentWatermark.watermark));

    if (tableNames.size() > 1) {
        addPreviousCacheTableParamsIfNecessary(tableNames[1], params);
    }
}

void CacheManager::addPreviousCacheTableParamsIfNecessary(const std::string& cacheTableName, std::map<std::string, std::string>& params) {
    params.emplace("previousCacheTableName", cacheTableName);
    auto previousWatermark = CacheWatermark<int64_t>::parseFromTableName(cacheTableName);
    params.emplace("previousWatermark", std::to_string(previousWatermark.watermark));
}

void CacheManager::performGarbageCollection(std::shared_ptr<ConfigManager> config_manager, const EndpointConfig& endpoint, const std::vector<std::string> previousTableNames) {
    const auto& cacheConfig = endpoint.cache;
    const auto& cacheSchema = config_manager->getCacheSchema();
    
    if (cacheConfig.maxPreviousTables == 0) {
        return;
    }

    if (previousTableNames.size() <= cacheConfig.maxPreviousTables) {
        return;  // No need for garbage collection
    }

    CROW_LOG_INFO << "Performing garbage collection for cache: " << cacheConfig.cacheTableName;

    // Keep the newest maxPreviousTables tables, drop the rest
    std::vector<std::string> tablesToDrop(previousTableNames.begin() + cacheConfig.maxPreviousTables, previousTableNames.end());

    if (!tablesToDrop.empty()) {
        // Construct the DROP TABLE statements
        std::stringstream dropQuery;
        dropQuery << "BEGIN TRANSACTION;";
        for (const auto& tableName : tablesToDrop) {
            dropQuery << "DROP TABLE IF EXISTS " << cacheSchema << "." << tableName << ";";
        }
        dropQuery << "COMMIT;";

        // Execute the DROP TABLE statements in a single transaction
        try {
            db_manager->executeQuery(dropQuery.str(), {}, false);
            CROW_LOG_INFO << "Dropped " << tablesToDrop.size() << " old cache tables for " << cacheConfig.cacheTableName;
        } catch (const std::exception& e) {
            CROW_LOG_ERROR << "Error during garbage collection: " << e.what();
        }
    }
}

std::optional<std::chrono::seconds> TimeInterval::parseInterval(const std::string& interval) {
    if (interval.empty()) {
        return std::nullopt;
    }

    try {
        std::regex pattern("^(\\d+)([smhd])$");
        std::smatch matches;
        
        if (!std::regex_match(interval, matches, pattern)) {
            return std::nullopt;
        }

        int value = std::stoi(matches[1].str());
        char unit = matches[2].str()[0];
        
        switch (unit) {
            case 's': return std::chrono::seconds(value);
            case 'm': return std::chrono::seconds(value * 60);
            case 'h': return std::chrono::seconds(value * 3600);
            case 'd': return std::chrono::seconds(value * 86400);
            default: return std::nullopt;
        }
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

} // namespace flapi
