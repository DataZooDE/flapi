#pragma once

#include <string>
#include <optional>
#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>
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
    std::thread warmUpCachesAsync(std::shared_ptr<ConfigManager> config_manager);
    bool shouldRefreshCache(std::shared_ptr<ConfigManager> config_manager, const EndpointConfig& endpoint);
    bool shouldRefreshCache(std::shared_ptr<ConfigManager> config_manager, const CacheConfig& cacheConfig);

    bool refreshCache(std::shared_ptr<ConfigManager> config_manager, const EndpointConfig& endpoint, std::map<std::string, std::string>& params);
    void refreshDuckLakeCache(std::shared_ptr<ConfigManager> config_manager, const EndpointConfig& endpoint, std::map<std::string, std::string> params);
    static std::string joinStrings(const std::vector<std::string>& values, const std::string& delimiter);
    void addQueryCacheParamsIfNecessary(std::shared_ptr<ConfigManager> config_manager, const EndpointConfig& endpoint, std::map<std::string, std::string>& params);
    void performGarbageCollection(std::shared_ptr<ConfigManager> config_manager, const EndpointConfig& endpoint, const std::vector<std::string> previousTableNames);

    enum class ReadinessState {
        Starting,
        Ready,
        Failed
    };

    struct CacheReadiness {
        ReadinessState state = ReadinessState::Ready;
        std::string catalog;
        std::string schema;
        std::string table;
        std::string error;
    };

    struct CacheReadinessSummary {
        int total = 0;
        int ready = 0;
        int failed = 0;
        std::vector<CacheReadiness> pending_caches;
        std::vector<CacheReadiness> failed_caches;
    };

    void initializeReadiness(std::shared_ptr<ConfigManager> config_manager);
    void markCacheStarting(std::shared_ptr<ConfigManager> config_manager, const EndpointConfig& endpoint);
    void markCacheReady(std::shared_ptr<ConfigManager> config_manager, const EndpointConfig& endpoint);
    void markCacheFailed(std::shared_ptr<ConfigManager> config_manager, const EndpointConfig& endpoint, const std::string& error);
    CacheReadiness getEndpointReadiness(std::shared_ptr<ConfigManager> config_manager, const EndpointConfig& endpoint) const;
    CacheReadinessSummary getReadinessSummary() const;

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

    struct CacheKey {
        std::string catalog;
        std::string schema;
        std::string table;

        bool operator<(const CacheKey& other) const;
    };

    static CacheKey cacheKeyForEndpoint(std::shared_ptr<ConfigManager> config_manager, const EndpointConfig& endpoint);
    CacheReadiness getReadinessForKey(const CacheKey& key) const;
    bool enterRefresh(const CacheKey& key);
    void leaveRefresh(const CacheKey& key);

    // Database adapter for cache operations (mockable for testing)
    std::shared_ptr<ICacheDatabaseAdapter> db_adapter_;

    // Keep db_manager for backward compatibility with code that accesses it directly
    std::shared_ptr<DatabaseManager> db_manager;

    mutable std::mutex readiness_mutex_;
    std::map<CacheKey, CacheReadiness> readiness_;

    mutable std::mutex inflight_mutex_;
    std::set<CacheKey> inflight_refreshes_;
};

} // namespace flapi
