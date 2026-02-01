#pragma once

#include <string>
#include <optional>
#include <chrono>
#include "config_manager.hpp"
#include "cache_database_adapter.hpp"
#include <duckdb.h>

namespace flapi {

class DatabaseManager; // Forward declaration

class CacheManager {
public:
    // Backward-compatible constructor: wraps DatabaseManager in adapter
    explicit CacheManager(std::shared_ptr<DatabaseManager> db_manager);

    // Constructor for unit testing: accepts mockable adapter directly
    explicit CacheManager(std::shared_ptr<ICacheDatabaseAdapter> db_adapter);

    void warmUpCaches(std::shared_ptr<ConfigManager> config_manager);
    bool shouldRefreshCache(std::shared_ptr<ConfigManager> config_manager, const EndpointConfig& endpoint);
    bool shouldRefreshCache(std::shared_ptr<ConfigManager> config_manager, const CacheConfig& cacheConfig);

    void refreshCache(std::shared_ptr<ConfigManager> config_manager, const EndpointConfig& endpoint, std::map<std::string, std::string>& params);
    void refreshDuckLakeCache(std::shared_ptr<ConfigManager> config_manager, const EndpointConfig& endpoint, std::map<std::string, std::string> params);
    static std::string joinStrings(const std::vector<std::string>& values, const std::string& delimiter);
    void addQueryCacheParamsIfNecessary(std::shared_ptr<ConfigManager> config_manager, const EndpointConfig& endpoint, std::map<std::string, std::string>& params);
    void performGarbageCollection(std::shared_ptr<ConfigManager> config_manager, const EndpointConfig& endpoint, const std::vector<std::string> previousTableNames);

    // Audit functionality
    void initializeAuditTables(std::shared_ptr<ConfigManager> config_manager);
    void ensureCacheSchemaExists(const std::string& catalog, const std::string& schema);
    void recordSyncEvent(std::shared_ptr<ConfigManager> config_manager, const EndpointConfig& endpoint, const std::string& sync_type, const std::string& status, const std::string& message = "");

private:
    struct SnapshotInfo {
        std::optional<std::string> current_snapshot_id;
        std::optional<std::string> current_snapshot_committed_at;
        std::optional<std::string> previous_snapshot_id;
        std::optional<std::string> previous_snapshot_committed_at;
    };

    SnapshotInfo fetchSnapshotInfo(const std::string& catalog, const std::string& schema, const std::string& table);
    static std::string determineCacheMode(const CacheConfig& cacheConfig);

    // Database adapter for cache operations (mockable for testing)
    std::shared_ptr<ICacheDatabaseAdapter> db_adapter_;

    // Keep db_manager for backward compatibility with code that accesses it directly
    std::shared_ptr<DatabaseManager> db_manager;
};

} // namespace flapi
