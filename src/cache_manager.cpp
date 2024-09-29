#include <sstream>
#include <fstream>
#include <stdexcept>
#include <crow.h>

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

    auto tableCreationTime = getTableCreationTime(config_manager->getCacheSchema(), newestCacheTable);
    auto now = std::chrono::system_clock::now();
    auto tableAge = std::chrono::duration_cast<std::chrono::seconds>(now - tableCreationTime);

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

std::string CacheManager::getCacheTableName(const CacheConfig& cacheConfig) {
    return createCacheTableName(cacheConfig.cacheTableName);
}

void CacheManager::refreshCache(std::shared_ptr<ConfigManager> config_manager, const EndpointConfig& endpoint, std::map<std::string, std::string>& params) {
    auto &cacheConfig = endpoint.cache;
    auto newCacheTableName = createCacheTableName(cacheConfig.cacheTableName);
    auto cacheSchema = config_manager->getCacheSchema();

    params.emplace("cacheSchema", cacheSchema);
    params.emplace("cacheTableName", newCacheTableName);
    params.emplace("cacheRefreshTime", cacheConfig.refreshTime);

    db_manager->executeCacheQuery(endpoint, cacheConfig, params);
    
    CROW_LOG_INFO << "Cache refreshed: " << config_manager->getCacheSchema() << "." << newCacheTableName;
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

    params.emplace("cacheSchema", config_manager->getCacheSchema());
    params.emplace("cacheTableName", tableNames.front());
    params.emplace("cacheRefreshTime", cacheConfig.refreshTime);
}

std::string CacheManager::createCacheTableName(const std::string& baseName) {
    auto now = std::chrono::system_clock::now();
    auto epoch = now.time_since_epoch();
    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(epoch);
    return baseName + "_" + std::to_string(seconds.count());
}

std::chrono::system_clock::time_point CacheManager::getTableCreationTime(const std::string& schema, const std::string& table) {
    // Extract the timestamp from the table name
    size_t pos = table.rfind('_');
    if (pos == std::string::npos) {
        throw std::runtime_error("Invalid cache table name format: " + table);
    }
    int64_t timestamp = std::stoll(table.substr(pos + 1));
    return std::chrono::system_clock::time_point(std::chrono::seconds(timestamp));
}

} // namespace flapi