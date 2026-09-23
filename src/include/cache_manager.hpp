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

    /// True when `value` is a plausible watermark for a cursor of
    /// `cursor_type`, and therefore safe to interpolate into a template that
    /// will be EXECUTED.
    ///
    /// The watermark comes from cached DATA, and cached data comes from
    /// upstream. It is rendered into the refresh template and run on the
    /// DuckLake connection, which has full DuckDB privileges - ATTACH,
    /// COPY ... TO, read_csv of local files. A VARCHAR cursor carrying
    /// partly upstream-controlled text (a page token, an `etag`) is therefore
    /// a second-order injection: one refresh ingests
    /// `x' UNION SELECT ... --`, it becomes max(cursor), and the NEXT
    /// refresh executes it.
    ///
    /// Escaping was tried and removed, correctly: the documented
    /// double-brace form HTML-escapes what it renders, so a pre-escaped value
    /// arrives mangled. Validation does not fight that - a value that passes
    /// needs no escaping, and one that fails is dropped, which puts the
    /// refresh on the full-load path it already has.
    static bool isPlausibleWatermark(const std::string& value,
                                     const std::string& cursor_type);

private:
    struct SnapshotInfo {
        std::optional<std::string> current_snapshot_id;
        std::optional<std::string> current_snapshot_committed_at;
        std::optional<std::string> previous_snapshot_id;
        std::optional<std::string> previous_snapshot_committed_at;
    };

    SnapshotInfo fetchSnapshotInfo(const std::string& catalog, const std::string& schema, const std::string& table);
    /// Double every `'` so a value is safe inside a SQL string literal.
    static std::string escapeSqlLiteral(const std::string& value);

    /// Quote a name as a SQL identifier.
    static std::string quoteIdentifier(const std::string& name);

    /// The largest cursor value actually present in the cache table, returned
    /// RAW - escaping belongs at interpolation, where the template author's
    /// choice of `{{{ }}}` versus `{{ }}` decides the quoting. Empty when the
    /// table does not exist yet, is empty, the cursor column is absent, or
    /// the value does not look like the declared cursor type.
    std::string fetchCursorWatermark(const std::string& catalog,
                                     const std::string& schema,
                                     const std::string& table,
                                     const std::string& cursor_column,
                                     const std::string& cursor_type);

    /// The names by which `catalog`'s `changes` map refers to
    /// `schema`.`table`: its numeric table id (for row changes) and
    /// `schema.table` (for creates).
    ///
    /// Resolved as its own query rather than inlined as a correlated
    /// subquery, because DuckDB refuses a subquery inside a lambda body
    /// ("Binder Error: subqueries in lambda expressions are not supported")
    /// and the exclusivity check needs a lambda.
    ///
    /// `strict` decides what an AMBIGUOUS table name means.
    ///
    /// Two schemas can hold the same table name, and ducklake_table_info()
    /// exposes no schema name to tell them apart (verified on DuckDB 1.5.5:
    /// two rows differing only in schema_id). For the destructive path -
    /// snapshot expiry - that must resolve to nothing, because acting on a
    /// guess deletes another endpoint's data. For the read-only watermark
    /// lookup it must NOT: returning nothing there silently downgrades every
    /// refresh to a full load, which is a different silent failure traded for
    /// the first. Best-effort keeps the pre-existing behaviour on a path that
    /// only ever read.
    std::vector<std::string> tableChangeKeys(const std::string& catalog,
                                             const std::string& schema,
                                             const std::string& table,
                                             bool strict);

    /// The `changes` keys of every table that currently EXISTS in the
    /// catalog. A key naming none of them is a dropped table's id, and must
    /// not make a snapshot count as shared with a live one.
    ///
    /// `std::nullopt` means the catalog could not be listed - NOT "there are
    /// no other tables". Those two must never collapse: an empty list makes
    /// the exclusivity filter accept every candidate, so failing open here
    /// expires another endpoint's shared snapshot on any transient
    /// introspection error. Expiry is destructive; it fails closed.
    std::optional<std::vector<std::string>> liveTableChangeKeys(const std::string& catalog);

    /// SQL predicate selecting snapshots that touched the table named by
    /// `keys`. ONE definition, used by both fetchSnapshotInfo and
    /// expirableSnapshotIds - they each had their own notion of "this table's
    /// snapshots" and only one of them was per-table, which is how retention
    /// came to destroy other endpoints' history.
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
    /// `keep_last` is taken BY VALUE because it is normalised inside: a
    /// configured 0 is cleared when an age policy accompanies it.
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
