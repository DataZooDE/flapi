#pragma once

#include <string>
#include <functional>
#include <optional>
#include <chrono>
#include <atomic>
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

/// How one cache refresh runs. At namespace scope: a nested struct with a default
/// member initializer cannot be a default argument inside its enclosing class.
struct CacheRefreshRequest {
    /// Rebuild from scratch: render with the watermark erased (the template's
    /// full-load branch) and drop the table first, in the same transaction.
    bool full = false;
};

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

    using RefreshOptions = CacheRefreshRequest;

    bool refreshCache(std::shared_ptr<ConfigManager> config_manager, const EndpointConfig& endpoint, std::map<std::string, std::string>& params, const RefreshOptions& options = {});
    void refreshDuckLakeCache(std::shared_ptr<ConfigManager> config_manager, const EndpointConfig& endpoint, std::map<std::string, std::string> params, const RefreshOptions& options = {});
    /// The populate SQL the NEXT refresh would run, with the live watermark and
    /// snapshot values - what `flapii templates expand --cache` shows. Fills
    /// `params` with the cache.* context it rendered with. Executes nothing.
    /// `full` renders it as a forced full rebuild would (no watermark).
    /// The params key renderRefreshTemplate sets (after rendering, never in the
    /// template context) when it dropped the cursor watermark, with the reason.
    static constexpr const char* kWatermarkDroppedReasonKey = "watermarkDroppedReason";
    std::string renderRefreshTemplate(std::shared_ptr<ConfigManager> config_manager, const EndpointConfig& endpoint,
                                      std::map<std::string, std::string>& params, bool full = false);
    /// The retention policy that applies to a cache: its own `retention` block when it
    /// sets either key, else the global `ducklake.retention`.
    static CacheConfig::RetentionConfig effectiveRetention(std::shared_ptr<ConfigManager> config_manager,
                                                           const CacheConfig& cacheConfig);
    /// The cached endpoints warm-up builds: one per cache table (endpoints sharing a
    /// table are built once, preferring one that names a populate template).
    static std::vector<EndpointConfig> warmupEndpoints(std::shared_ptr<ConfigManager> config_manager);

    /// When this process last finished a refresh of the endpoint's cache table
    /// (success or failure), if it did.
    std::optional<std::chrono::system_clock::time_point> lastRefreshCompletedAt(
        std::shared_ptr<ConfigManager> config_manager, const EndpointConfig& endpoint) const;
    /// Commit time of the newest snapshot that touched the endpoint's cache table.
    std::optional<std::chrono::system_clock::time_point> newestSnapshotTime(
        std::shared_ptr<ConfigManager> config_manager, const EndpointConfig& endpoint);
    /// How old the SERVED data of catalog.schema.table is: the last successful
    /// refresh this process ran, else the newest snapshot in the catalog (looked
    /// up once and remembered). After a restart whose first refresh failed the
    /// old snapshot is still served, and its age used to be reported as absent.
    std::optional<std::chrono::system_clock::time_point> dataTimestamp(
        const std::string& catalog, const std::string& schema, const std::string& table);

    /// Catalog-wide compaction (merge adjacent files, rewrite delete-heavy files,
    /// delete the replaced files), recorded in the audit log as `compaction`.
    void performCompaction(std::shared_ptr<ConfigManager> config_manager);
    /// When compaction last ran, from the audit log.
    std::optional<std::chrono::system_clock::time_point> lastCompactionTime(std::shared_ptr<ConfigManager> config_manager);
    /// ducklake.compaction is enabled and its schedule has elapsed since the last run.
    bool compactionDue(std::shared_ptr<ConfigManager> config_manager, std::chrono::system_clock::time_point now);

    /// Shutdown: refuse new refreshes, stop warm-up, and interrupt the populate
    /// queries running now (repeatedly, for up to `budget`).
    void interruptRefreshesForShutdown(std::chrono::milliseconds budget = std::chrono::milliseconds(5000));
    bool shutdownRequested() const;

    /// Bound the audit log: keep the newest kAuditRowsKept rows per cache table and
    /// expire all but kAuditSnapshotsKept of the audit table's snapshots.
    void pruneAuditLog(std::shared_ptr<ConfigManager> config_manager);
    static constexpr std::size_t kAuditRowsKept = 1000;
    static constexpr std::size_t kAuditSnapshotsKept = 1;
    static constexpr int kAuditPruneEvery = 100;
    /// A random RFC 4122 version-4 UUID: the audit event id.
    static std::string newEventId();
    static std::string joinStrings(const std::vector<std::string>& values, const std::string& delimiter);
    void addQueryCacheParamsIfNecessary(std::shared_ptr<ConfigManager> config_manager, const EndpointConfig& endpoint, std::map<std::string, std::string>& params);
    /// What a garbage collection did, for the GC route's JSON answer.
    struct GarbageCollectionResult {
        /// expired | nothing_to_expire | no_policy | error
        std::string status;
        std::size_t expired_snapshots = 0;
        std::size_t files_deleted = 0;
        std::int64_t bytes_freed = 0;
        std::string message;
        std::string warning;
    };
    /// Drop a cache table no endpoint uses, expire its snapshots and delete its
    /// files. status: dropped | not_found.
    GarbageCollectionResult dropObsoleteCacheTable(std::shared_ptr<ConfigManager> config_manager,
                                                   const std::string& schema, const std::string& table);
    GarbageCollectionResult performGarbageCollection(std::shared_ptr<ConfigManager> config_manager, const EndpointConfig& endpoint, const std::vector<std::string> previousTableNames);

    enum class ReadinessState {
        Starting,
        Ready,
        Failed,
        Unbuilt   // warm-up skipped and the table does not exist yet
    };

    struct CacheReadiness {
        ReadinessState state = ReadinessState::Ready;
        std::string catalog;
        std::string schema;
        std::string table;
        std::string error;       // internal: logged, never returned to callers
        bool stale = false;      // Ready, but the latest refresh failed (last snapshot still served)
        bool previous_table = false;  // Starting over a table a previous run built (warm-up: always)
    };

    struct CacheReadinessSummary {
        int total = 0;
        int ready = 0;
        int failed = 0;
        int stale = 0;
        int unbuilt = 0;
        std::vector<CacheReadiness> pending_caches;
        std::vector<CacheReadiness> failed_caches;
        std::vector<CacheReadiness> stale_caches;
        std::vector<CacheReadiness> unbuilt_caches;
    };

    void initializeReadiness(std::shared_ptr<ConfigManager> config_manager);
    void markCacheStarting(std::shared_ptr<ConfigManager> config_manager, const EndpointConfig& endpoint);
    void markCacheReady(std::shared_ptr<ConfigManager> config_manager, const EndpointConfig& endpoint);
    void markCacheFailed(std::shared_ptr<ConfigManager> config_manager, const EndpointConfig& endpoint, const std::string& error);
    void markCacheUnbuilt(std::shared_ptr<ConfigManager> config_manager, const EndpointConfig& endpoint);
    // Whether the cache table exists in the DuckLake catalog. A failed lookup reads as "no".
    bool cacheTableExists(const std::string& catalog, const std::string& schema, const std::string& table);
    CacheReadiness getEndpointReadiness(std::shared_ptr<ConfigManager> config_manager, const EndpointConfig& endpoint) const;
    /// Non-const: on a read-only replica an unbuilt cache is re-checked here
    /// (throttled) - the writer may have built it since.
    std::optional<CacheReadiness> readinessBlock(std::shared_ptr<ConfigManager> config_manager, const EndpointConfig& endpoint);
    /// Read-only replicas only: every cache not Ready (unbuilt, failed, or
    /// starting with no refresh in flight) whose table now exists in the shared
    /// catalog becomes Ready. `force` skips the per-table throttle. Returns how
    /// many became Ready. A no-op on a writer, whose own refreshes set readiness.
    std::size_t recheckReadOnlyCaches(std::shared_ptr<ConfigManager> config_manager, bool force = false);
    /// Minimum time between two re-checks of one table on the request path.
    static constexpr std::chrono::seconds kReadOnlyRecheckInterval{5};
    static crow::json::wvalue readinessBlockJson(const CacheReadiness& readiness);
    static crow::response readinessBlockResponse(const CacheReadiness& readiness);
    CacheReadinessSummary getReadinessSummary() const;

    // Audit functionality
    void initializeAuditTables(std::shared_ptr<ConfigManager> config_manager);
    void ensureCacheSchemaExists(const std::string& catalog, const std::string& schema);
    /// What a sync event measured. Every field is optional: an event that
    /// failed before its write ran has no row count, and a GC has no snapshot.
    struct SyncEventDetails {
        std::optional<std::chrono::system_clock::time_point> started_at;
        std::optional<std::chrono::system_clock::time_point> completed_at;
        std::optional<int64_t> rows_affected;
        std::optional<std::string> snapshot_id;
        // Set when the refresh ran but not as configured - today: the cursor
        // watermark was dropped (cursor.type mismatch) and it loaded the full
        // source. Carried into the audit message and the refresh answer.
        std::optional<std::string> warning;
    };
    void recordSyncEvent(std::shared_ptr<ConfigManager> config_manager, const EndpointConfig& endpoint, const std::string& sync_type, const std::string& status, const std::string& message = "", const SyncEventDetails& details = {});
    /// In-memory roll-up of every refresh this process ran, per cache table,
    /// for GET /api/v1/_config/metrics. Kept beside the audit table rather than
    /// read from it so the metrics endpoint never queries DuckLake.
    struct CacheRefreshStats {
        std::string endpoint_path;
        std::string catalog;
        std::string schema;
        std::string table;
        std::string last_sync_type;
        std::string last_status;
        std::optional<int64_t> last_duration_ms;
        std::optional<int64_t> last_rows_affected;
        std::optional<std::string> last_snapshot_id;
        std::optional<std::chrono::system_clock::time_point> last_completed_at;
        std::optional<std::chrono::system_clock::time_point> last_success_at;
        std::optional<std::string> last_warning;
        int64_t refreshes = 0;
        int64_t failures = 0;
        int64_t total_duration_ms = 0;
        int64_t max_duration_ms = 0;
    };
    std::vector<CacheRefreshStats> getRefreshStats() const;
    /// Readiness of every cache as a JSON-ready state name
    /// (starting | ready | stale | failed | unbuilt), keyed by catalog.schema.table.
    std::map<std::string, std::string> getReadinessStates() const;

    /// The INSERT that records one sync event. Pure, for tests.
    static std::string buildSyncEventInsert(const std::string& catalog, const std::string& event_id,
                                            const EndpointConfig& endpoint, const std::string& sync_type,
                                            const std::string& status, const std::string& message,
                                            const SyncEventDetails& details);
    /// `YYYY-MM-DD HH:MM:SS.mmm` in UTC, the form a DuckDB TIMESTAMP literal takes.
    static std::string formatUtcTimestamp(std::chrono::system_clock::time_point tp);

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
    /// Delete data files no remaining snapshot references (after an expiry).
    std::size_t cleanupOldFiles(const std::string& catalog);
    /// Total bytes of the data and delete files the catalog still references.
    std::optional<std::int64_t> referencedFileBytes(const std::string& catalog);

    /// What one retention pass did: run after a refresh and by manual GC.
    struct RetentionOutcome {
        std::size_t expired_snapshots = 0;
        std::size_t files_deleted = 0;
        std::int64_t bytes_freed = 0;
        /// Set when the policy could not do what it says (shared snapshots, an
        /// unresolvable table); logged at WARNING and put in the audit message.
        std::string warning;
        std::string error;
    };
    /// `before_expiry` runs once the candidates are known (with the warning, if
    /// any) and before anything is expired.
    RetentionOutcome applyRetention(const std::string& catalog, const std::string& schema,
                                    const std::string& table, const CacheConfig::RetentionConfig& retention,
                                    const std::function<void(const std::string&)>& before_expiry = {});
    /// Expire all but the newest kAuditSnapshotsKept audit snapshots; true if it ran.
    bool expireAuditSnapshots(const std::string& catalog);

    /// Publishes the current thread as running a populate script, for shutdown.
    class ActiveRefreshGuard {
    public:
        explicit ActiveRefreshGuard(CacheManager& manager);
        ~ActiveRefreshGuard();
        ActiveRefreshGuard(const ActiveRefreshGuard&) = delete;
        ActiveRefreshGuard& operator=(const ActiveRefreshGuard&) = delete;

    private:
        CacheManager& manager_;
        std::thread::id tid_;
    };

    std::atomic<bool> shutdown_requested_{false};
    mutable std::mutex active_refresh_mutex_;
    std::set<std::thread::id> active_refresh_threads_;
    std::atomic<int> audit_events_since_prune_{0};

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

    /// The (quoted) name of the metadata database DuckLake attaches for `catalog`.
    static std::string metadataCatalog(const std::string& catalog);

    /// How `changes.tables_created` names schema.table: the bare form, plus the
    /// form DuckLake actually writes (each part optionally quoted, so the default
    /// schema `cache` - a keyword - appears as '"cache".t').
    static std::vector<std::string> tableNameKeys(const std::string& schema, const std::string& table);

    /// The largest cursor value actually present in the cache table, returned
    /// RAW - escaping belongs at interpolation, where the template author's
    /// choice of `{{{ }}}` versus `{{ }}` decides the quoting. Empty when the
    /// table does not exist yet, is empty, the cursor column is absent, or
    /// the value does not look like the declared cursor type.
    std::string fetchCursorWatermark(const std::string& catalog,
                                     const std::string& schema,
                                     const std::string& table,
                                     const std::string& cursor_column,
                                     const std::string& cursor_type,
                                     std::string* dropped_reason = nullptr);

    /// The names by which `catalog`'s `changes` map refers to
    /// `schema`.`table`: its numeric table id (for row changes) and
    /// `schema.table` (for creates). Every id the table has had - a full refresh
    /// (CREATE OR REPLACE) and a `--full` rebuild each make a new one - resolved
    /// through the schema NAME from the metadata catalog's ducklake_table /
    /// ducklake_schema, so dropped incarnations stay expirable and a same-named
    /// table in another schema is never confused with this one.
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
        /// The table could not be resolved unambiguously, so nothing was considered.
        bool unresolved = false;
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
                               const std::string& older_than_sql,
                               ExpiryCandidates* report = nullptr);

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
    // Read-only replicas: when each table was last re-checked (readiness_mutex_).
    std::map<CacheKey, std::chrono::steady_clock::time_point> readonly_rechecked_at_;
    /// One table's read-only re-check; true when it became Ready.
    bool recheckReadOnlyKey(const CacheKey& key, bool force);

    void updateRefreshStats(const CacheKey& key, const std::string& endpoint_path, const std::string& sync_type,
                            const std::string& status, const SyncEventDetails& details);
    mutable std::mutex refresh_stats_mutex_;
    std::map<CacheKey, CacheRefreshStats> refresh_stats_;
    // dataTimestamp's catalog lookups, by table; an entry may hold nullopt.
    std::map<CacheKey, std::optional<std::chrono::system_clock::time_point>> snapshot_time_memo_;
    std::optional<std::chrono::system_clock::time_point> newestSnapshotTimeForKey(const CacheKey& key);

    mutable std::mutex inflight_mutex_;
    std::set<CacheKey> inflight_refreshes_;
};

} // namespace flapi
