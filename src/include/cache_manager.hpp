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
    std::optional<CacheReadiness> readinessBlock(std::shared_ptr<ConfigManager> config_manager, const EndpointConfig& endpoint) const;
    static crow::json::wvalue readinessBlockJson(const CacheReadiness& readiness);
    static crow::response readinessBlockResponse(const CacheReadiness& readiness);
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
    /// SQL to expire every snapshot beyond the newest `keep_last`, or an empty
    /// string when there is nothing to expire. Resolves the ids first because
    /// `versions` takes an explicit list.
    /// Double every `'` so a value is safe inside a SQL string literal.
    static std::string escapeSqlLiteral(const std::string& value);

    /// Quote a name as a SQL identifier.
    static std::string quoteIdentifier(const std::string& name);

    /// The names by which `catalog`'s `changes` map refers to
    /// `schema`.`table`: its numeric table id (for row changes) and
    /// `schema.table` (for creates). Empty if neither can be resolved.
    ///
    /// Resolved as its own query rather than inlined as a correlated
    /// subquery, because DuckDB refuses a subquery inside a lambda body
    /// ("Binder Error: subqueries in lambda expressions are not supported")
    /// and the exclusivity check below needs a lambda.
    /// The largest cursor value actually present in the cache table, as a
    /// string ready to interpolate. Empty when the table does not exist yet,
    /// is empty, or the cursor column is absent.
    std::string fetchCursorWatermark(const std::string& catalog,
                                     const std::string& schema,
                                     const std::string& table,
                                     const std::string& cursor_column);

    std::vector<std::string> tableChangeKeys(const std::string& catalog,
                                             const std::string& schema,
                                             const std::string& table);

    /// SQL predicate selecting snapshots that touched the table named by
    /// `keys`. One definition, used by both fetchSnapshotInfo and
    /// buildCountBasedExpireSql - they each had their own notion of "this
    /// table's snapshots" and only one of them was per-table.
    static std::string tableSnapshotPredicate(const std::vector<std::string>& keys);

    /// Snapshot ids of `schema`.`table` that this endpoint may expire.
    ///
    /// `keep_last` retains that many newest; `older_than_sql` (a SQL
    /// expression) retains anything at or after it. Either may be empty.
    /// Only snapshots that touched NOTHING BUT this table are returned - see
    /// buildExpireSql.
    struct ExpiryCandidates {
        /// Snapshots this endpoint may expire.
        std::vector<std::int64_t> expirable;
        /// Snapshots of this table that a policy would have expired but that
        /// are SHARED with another table, so expiring them would discard its
        /// data too. Reported separately because "nothing to expire" and
        /// "nothing expirable" look identical from the outside, and the
        /// second means a configured policy silently never fires.
        std::size_t shared = 0;
    };

    ExpiryCandidates expirableSnapshotIds(const std::string& catalog,
                                          const std::string& schema,
                                          const std::string& table,
                                          std::optional<std::size_t> keep_last,
                                          const std::string& older_than_sql);

    /// `CALL ducklake_expire_snapshots(..., versions => [...])`, or empty when
    /// there is nothing this endpoint may expire.
    ///
    /// EVERY expiry goes through here. ducklake_expire_snapshots with
    /// `older_than =>` acts on the whole CATALOG, and every cached endpoint
    /// shares one - so an age-based policy or a manual GC on one endpoint
    /// destroyed every other endpoint's history and incremental watermark.
    /// Expiring by explicit, per-table version ids is the only safe form.
    std::string buildExpireSql(const std::string& catalog,
                               const std::string& schema,
                               const std::string& table,
                               std::optional<std::size_t> keep_last,
                               const std::string& older_than_sql);

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
