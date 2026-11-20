#include <algorithm>
#include <cctype>
#include <sstream>
#include <functional>
#include <locale>
#include <fstream>
#include <stdexcept>
#include <crow.h>
#include <regex>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <random>
#include <tuple>

#include "cache_manager.hpp"
#include "template_secrets.hpp"

#include "database_manager.hpp"
#include "database_manager_cache_adapter.hpp"
#include "query_executor.hpp"

#include "duckdb/parser/keyword_helper.hpp"

namespace flapi {

bool CacheManager::CacheKey::operator<(const CacheKey& other) const {
    return std::tie(catalog, schema, table) < std::tie(other.catalog, other.schema, other.table);
}

CacheManager::CacheManager(std::shared_ptr<DatabaseManager> db_manager)
    : db_adapter_(std::make_shared<DatabaseManagerCacheAdapter>(db_manager)),
      db_manager(db_manager) {}

CacheManager::CacheManager(std::shared_ptr<ICacheDatabaseAdapter> db_adapter)
    : db_adapter_(std::move(db_adapter)),
      db_manager(nullptr) {}

void CacheManager::warmUpCaches(std::shared_ptr<ConfigManager> config_manager) {
    CROW_LOG_INFO << "Warming up endpoint caches, this might take some time...";
    initializeReadiness(config_manager);
    
    // Initialize audit tables first - not on a read-only catalog, which cannot take the DDL.
    if (!config_manager->getDuckLakeConfig().read_only) {
        initializeAuditTables(config_manager);
    }
    
    const auto& cache_schema = config_manager->getCacheSchema();

    std::map<std::string, std::string> params;
    for (const auto& endpoint : warmupEndpoints(config_manager))
    {
        if (shutdown_requested_.load()) {
            CROW_LOG_INFO << "Shutdown requested; abandoning the remaining cache warm-up";
            break;
        }
        {
            const auto mode = config_manager->getEffectiveCacheWarmupMode(endpoint);
            if (mode != CacheWarmupMode::Always) {
                const CacheKey key = cacheKeyForEndpoint(config_manager, endpoint);
                if (cacheTableExists(key.catalog, key.schema, key.table)) {
                    CROW_LOG_INFO << "Cache " << key.schema << "." << key.table
                                  << " already exists; skipping warm-up (" << CacheWarmupModeToString(mode) << ")";
                    markCacheReady(config_manager, endpoint);
                    continue;
                }
                if (mode == CacheWarmupMode::Never) {
                    CROW_LOG_WARNING << "Cache " << key.schema << "." << key.table
                                     << " does not exist and warm-up is 'never'; its endpoint answers 503 "
                                        "cache_not_built until a refresh builds it";
                    markCacheUnbuilt(config_manager, endpoint);
                    continue;
                }
            }
            markCacheStarting(config_manager, endpoint);
            if (mode == CacheWarmupMode::Always) {
                // A restart while the source is down must not take the endpoint
                // offline when DuckLake still holds the table a previous run built:
                // a failed rebuild then leaves it served as `stale`, exactly as a
                // failed scheduled refresh does. It used to become `failed` (503).
                const CacheKey key = cacheKeyForEndpoint(config_manager, endpoint);
                if (cacheTableExists(key.catalog, key.schema, key.table)) {
                    std::lock_guard<std::mutex> lock(readiness_mutex_);
                    readiness_[key].previous_table = true;
                }
            }
            try {
                if (refreshCache(config_manager, endpoint, params)) {
                    markCacheReady(config_manager, endpoint);
                } else {
                    while (getEndpointReadiness(config_manager, endpoint).state == ReadinessState::Starting) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                }
            } catch (const std::exception&) {
                // Already marked failed and logged by refreshCache(). Logging and
                // marking again here wrote "Cache build failed" twice per failure.
            } catch (...) {
            }
        }
    }
    CROW_LOG_INFO << "Finished warming up endpoint caches! Let's go!";
}

std::vector<EndpointConfig> CacheManager::warmupEndpoints(std::shared_ptr<ConfigManager> config_manager) {
    // ONE warm-up per cache table. A GET endpoint and the write endpoint beside it
    // commonly share a cache table, and each used to build it in turn - the same
    // populate query two or three times at startup. Readiness is keyed by table
    // already, so the duplicates bought nothing.
    //
    // When several endpoints share a table, the one that names a populate
    // template wins; otherwise the first in load order.
    std::vector<EndpointConfig> out;
    std::map<CacheKey, std::size_t> index;
    const auto endpoints = config_manager->getEndpoints();
    for (const auto& endpoint : *endpoints) {
        if (!endpoint.cache.enabled || endpoint.cache.table.empty()) {
            continue;
        }
        const CacheKey key = cacheKeyForEndpoint(config_manager, endpoint);
        const auto it = index.find(key);
        if (it == index.end()) {
            index.emplace(key, out.size());
            out.push_back(endpoint);
            continue;
        }
        if (!out[it->second].cache.hasTemplate() && endpoint.cache.hasTemplate()) {
            out[it->second] = endpoint;
        }
        CROW_LOG_DEBUG << "Cache " << key.schema << "." << key.table << " is shared by "
                       << endpoint.urlPath << "; warmed once";
    }
    return out;
}

std::thread CacheManager::warmUpCachesAsync(std::shared_ptr<ConfigManager> config_manager) {
    initializeReadiness(config_manager);
    return std::thread([this, config_manager]() {
        try {
            warmUpCaches(config_manager);
        } catch (const std::exception& ex) {
            CROW_LOG_ERROR << "Unexpected cache warmup failure: " << ex.what();
        } catch (...) {
            CROW_LOG_ERROR << "Unexpected cache warmup failure: unknown error";
        }
    });
}

bool CacheManager::shouldRefreshCache(std::shared_ptr<ConfigManager> config_manager, const EndpointConfig& endpoint) {
    // Do not refresh cache on regular request execution path.
    // Refreshes happen during warmup, scheduled tasks, or explicit manual triggers.
    return false;
}

bool CacheManager::shouldRefreshCache(std::shared_ptr<ConfigManager> config_manager, const CacheConfig& cacheConfig) {
        // Cache should only be refreshed during:
        // 1. Initial warmup (handled separately)
        // 2. Scheduled refreshes (handled by HeartbeatWorker)
        // 3. Manual refresh requests (handled by ConfigService)
        //
        // Regular endpoint requests should NOT trigger cache refresh
        return false;
    }

bool CacheManager::refreshCache(std::shared_ptr<ConfigManager> config_manager, const EndpointConfig& endpoint, std::map<std::string, std::string>& params, const RefreshOptions& options) {
    const CacheKey key = cacheKeyForEndpoint(config_manager, endpoint);
    if (!enterRefresh(key)) {
        CROW_LOG_INFO << "Skipping duplicate in-flight cache refresh for " << key.schema << "." << key.table;
        return false;
    }

    try {
        // After the duplicate check, so a second request for the SAME table is
        // coalesced rather than queued; a refresh of another table waits here.
        auto catalog_lock = lockCatalogWrites();
        refreshDuckLakeCache(config_manager, endpoint, params, options);
        catalog_lock.unlock();
        markCacheReady(config_manager, endpoint);
        leaveRefresh(key);
        return true;
    } catch (const std::exception& ex) {
        markCacheFailed(config_manager, endpoint, ex.what());
        leaveRefresh(key);
        throw;
    } catch (...) {
        markCacheFailed(config_manager, endpoint, "unknown error");
        leaveRefresh(key);
        throw;
    }
}

CacheManager::CacheKey CacheManager::cacheKeyForEndpoint(std::shared_ptr<ConfigManager> config_manager, const EndpointConfig& endpoint) {
    const auto& ducklakeConfig = config_manager->getDuckLakeConfig();
    const auto& cacheConfig = endpoint.cache;
    return CacheKey{
        ducklakeConfig.alias,
        cacheConfig.schema.empty() ? "main" : cacheConfig.schema,
        cacheConfig.table
    };
}

void CacheManager::initializeReadiness(std::shared_ptr<ConfigManager> config_manager) {
    std::lock_guard<std::mutex> lock(readiness_mutex_);
    readiness_.clear();
    for (const auto& endpoint : *config_manager->getEndpoints()) {
        if (!endpoint.cache.enabled || endpoint.cache.table.empty()) {
            continue;
        }
        const CacheKey key = cacheKeyForEndpoint(config_manager, endpoint);
        readiness_[key] = CacheReadiness{ReadinessState::Starting, key.catalog, key.schema, key.table, ""};
    }
    CROW_LOG_INFO << "Cache readiness initialised for " << readiness_.size() << " cache table(s)";
}

void CacheManager::syncReadinessWithEndpoints(std::shared_ptr<ConfigManager> config_manager) {
    std::set<CacheKey> named;
    for (const auto& endpoint : *config_manager->getEndpoints()) {
        if (endpoint.cache.enabled && !endpoint.cache.table.empty()) {
            named.insert(cacheKeyForEndpoint(config_manager, endpoint));
        }
    }
    std::vector<CacheKey> added;
    {
        std::lock_guard<std::mutex> lock(readiness_mutex_);
        for (auto it = readiness_.begin(); it != readiness_.end();) {
            if (named.count(it->first) == 0) {
                CROW_LOG_INFO << "Cache " << it->first.schema << "." << it->first.table
                              << " is no longer used by any endpoint; dropped from readiness";
                readonly_rechecked_at_.erase(it->first);
                it = readiness_.erase(it);
            } else {
                ++it;
            }
        }
        for (const auto& key : named) {
            if (readiness_.count(key) == 0) {
                added.push_back(key);
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(refresh_stats_mutex_);
        for (auto it = refresh_stats_.begin(); it != refresh_stats_.end();) {
            it = named.count(it->first) == 0 ? refresh_stats_.erase(it) : std::next(it);
        }
        for (auto it = snapshot_time_memo_.begin(); it != snapshot_time_memo_.end();) {
            it = named.count(it->first) == 0 ? snapshot_time_memo_.erase(it) : std::next(it);
        }
    }
    for (const auto& key : added) {
        // Outside the lock: a catalog query.
        const bool exists = cacheTableExists(key.catalog, key.schema, key.table);
        std::lock_guard<std::mutex> lock(readiness_mutex_);
        if (readiness_.count(key) != 0) {
            continue;   // a refresh got there first
        }
        readiness_[key] = CacheReadiness{exists ? ReadinessState::Ready : ReadinessState::Unbuilt, key.catalog,
                                         key.schema, key.table, ""};
        CROW_LOG_INFO << "Cache " << key.schema << "." << key.table << " is new to this server: "
                      << (exists ? "the table exists, serving it" : "not built yet (503 cache_not_built until a refresh)");
    }
}

void CacheManager::markCacheStarting(std::shared_ptr<ConfigManager> config_manager, const EndpointConfig& endpoint) {
    const CacheKey key = cacheKeyForEndpoint(config_manager, endpoint);
    std::lock_guard<std::mutex> lock(readiness_mutex_);
    readiness_[key] = CacheReadiness{ReadinessState::Starting, key.catalog, key.schema, key.table, ""};
}

void CacheManager::markCacheReady(std::shared_ptr<ConfigManager> config_manager, const EndpointConfig& endpoint) {
    const CacheKey key = cacheKeyForEndpoint(config_manager, endpoint);
    std::lock_guard<std::mutex> lock(readiness_mutex_);
    readiness_[key] = CacheReadiness{ReadinessState::Ready, key.catalog, key.schema, key.table, ""};
}

void CacheManager::markCacheFailed(std::shared_ptr<ConfigManager> config_manager, const EndpointConfig& endpoint, const std::string& error) {
    const CacheKey key = cacheKeyForEndpoint(config_manager, endpoint);
    std::lock_guard<std::mutex> lock(readiness_mutex_);
    // A refresh that fails over a cache that was already built must not take the endpoint
    // offline: the last snapshot is still there and still correct, just not fresh. Only a
    // cache that never built (Starting) becomes Failed (HTTP 503).
    const auto previous = readiness_.find(key);
    const bool was_built = previous != readiness_.end() &&
                           (previous->second.state == ReadinessState::Ready || previous->second.previous_table);
    if (was_built) {
        auto stale = previous->second;
        stale.state = ReadinessState::Ready;
        stale.previous_table = false;
        stale.stale = true;
        stale.error = error;
        readiness_[key] = std::move(stale);
        CROW_LOG_ERROR << "Cache refresh failed for " << key.schema << "." << key.table
                       << "; still serving the last snapshot: " << error;
        return;
    }
    readiness_[key] = CacheReadiness{ReadinessState::Failed, key.catalog, key.schema, key.table, error};
    CROW_LOG_ERROR << "Cache build failed for " << key.schema << "." << key.table << ": " << error;
}

void CacheManager::markCacheUnbuilt(std::shared_ptr<ConfigManager> config_manager, const EndpointConfig& endpoint) {
    const CacheKey key = cacheKeyForEndpoint(config_manager, endpoint);
    std::lock_guard<std::mutex> lock(readiness_mutex_);
    readiness_[key] = CacheReadiness{ReadinessState::Unbuilt, key.catalog, key.schema, key.table, ""};
}

bool CacheManager::cacheTableExists(const std::string& catalog, const std::string& schema, const std::string& table) {
    try {
        // Filtered here rather than in SQL: the listing is small, and the same
        // shape is what the retention code already reads from duckdb_tables().
        auto result = db_adapter_->executeDuckLakeQueryWithResult(
            "SELECT schema_name, table_name FROM duckdb_tables() WHERE database_name = '" +
            escapeSqlLiteral(catalog) + "'");
        auto rows = crow::json::load(result.data.dump());
        if (!rows || rows.t() != crow::json::type::List) {
            return false;
        }
        for (const auto& row : rows) {
            if (row.has("schema_name") && row.has("table_name") &&
                row["schema_name"].s() == schema && row["table_name"].s() == table) {
                return true;
            }
        }
    } catch (const std::exception& ex) {
        CROW_LOG_WARNING << "Could not list tables in catalog '" << catalog << "': " << ex.what();
    }
    return false;
}

CacheManager::CacheReadiness CacheManager::getReadinessForKey(const CacheKey& key) const {
    std::lock_guard<std::mutex> lock(readiness_mutex_);
    auto it = readiness_.find(key);
    if (it == readiness_.end()) {
        return CacheReadiness{ReadinessState::Ready, key.catalog, key.schema, key.table, ""};
    }
    return it->second;
}

CacheManager::CacheReadiness CacheManager::getEndpointReadiness(std::shared_ptr<ConfigManager> config_manager, const EndpointConfig& endpoint) const {
    if (!endpoint.cache.enabled || endpoint.cache.table.empty()) {
        return CacheReadiness{ReadinessState::Ready, "", "", "", ""};
    }
    return getReadinessForKey(cacheKeyForEndpoint(config_manager, endpoint));
}

std::optional<CacheManager::CacheReadiness> CacheManager::readinessBlock(std::shared_ptr<ConfigManager> config_manager, const EndpointConfig& endpoint) {
    if (!endpoint.cache.enabled || endpoint.cache.table.empty()) {
        return std::nullopt;
    }

    auto readiness = getEndpointReadiness(config_manager, endpoint);
    if (readiness.state == ReadinessState::Ready) {
        return std::nullopt;
    }
    // A read-only replica never refreshes, so nothing local ever clears Unbuilt:
    // a replica started before the writer built the table answered 503
    // cache_not_built FOREVER. The writer may have built it since - look.
    if (config_manager->getDuckLakeConfig().read_only &&
        recheckReadOnlyKey(cacheKeyForEndpoint(config_manager, endpoint), /*force=*/false)) {
        return std::nullopt;
    }
    readiness.replica = config_manager->getDuckLakeConfig().read_only;
    return readiness;
}

bool CacheManager::recheckReadOnlyKey(const CacheKey& key, bool force) {
    {
        std::lock_guard<std::mutex> lock(readiness_mutex_);
        const auto it = readiness_.find(key);
        if (it == readiness_.end() || it->second.state == ReadinessState::Ready) {
            return false;
        }
        const auto now = std::chrono::steady_clock::now();
        auto& last = readonly_rechecked_at_[key];
        // Throttled: every request to an unbuilt cache would otherwise list the
        // catalog's tables.
        if (!force && last.time_since_epoch().count() != 0 && now - last < kReadOnlyRecheckInterval) {
            return false;
        }
        last = now;
    }
    {
        // A table whose warm-up is still running is the warm-up's to settle.
        std::lock_guard<std::mutex> lock(inflight_mutex_);
        if (inflight_refreshes_.count(key) > 0) {
            return false;
        }
    }
    if (!cacheTableExists(key.catalog, key.schema, key.table)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(readiness_mutex_);
    auto it = readiness_.find(key);
    if (it == readiness_.end() || it->second.state == ReadinessState::Ready) {
        return false;
    }
    it->second = CacheReadiness{ReadinessState::Ready, key.catalog, key.schema, key.table, ""};
    CROW_LOG_INFO << "Cache " << key.schema << "." << key.table
                  << " now exists in the shared catalog (built by the writer); serving it";
    return true;
}

std::size_t CacheManager::recheckReadOnlyCaches(std::shared_ptr<ConfigManager> config_manager, bool force) {
    if (!config_manager->getDuckLakeConfig().read_only) {
        return 0;
    }
    std::vector<CacheKey> keys;
    {
        std::lock_guard<std::mutex> lock(readiness_mutex_);
        for (const auto& [key, readiness] : readiness_) {
            if (readiness.state != ReadinessState::Ready) {
                keys.push_back(key);
            }
        }
    }
    std::size_t became_ready = 0;
    for (const auto& key : keys) {
        if (recheckReadOnlyKey(key, force)) {
            ++became_ready;
        }
    }
    return became_ready;
}

crow::json::wvalue CacheManager::readinessBlockJson(const CacheReadiness& readiness) {
    crow::json::wvalue errorResponse;
    // Three situations, three names: a client (or an operator reading a 503) must be
    // able to tell "wait" from "this will not fix itself".
    errorResponse["error"] = readiness.state == ReadinessState::Unbuilt  ? "cache_not_built"
                           : readiness.state == ReadinessState::Failed   ? "cache_build_failed"
                                                                         : "cache_warming";
    errorResponse["table"] = readiness.table;
    if (readiness.state == ReadinessState::Unbuilt && readiness.replica) {
        // A replica never builds a cache: "warm-up skipped" sent operators
        // looking for a warm-up setting on an instance that cannot warm up.
        errorResponse["message"] = "Cache for this endpoint has not been built yet: this is a read-only replica, "
                                   "and the writer has not built it yet";
    } else if (readiness.state == ReadinessState::Unbuilt) {
        errorResponse["message"] = "Cache for this endpoint has not been built (warm-up skipped)";
    } else if (readiness.state == ReadinessState::Failed) {
        // No `detail`: it carried the raw DuckDB error (SQL, file paths, connection
        // strings) to anonymous callers. The error is in the server log.
        errorResponse["message"] = "Cache for this endpoint failed to build";
    } else {
        errorResponse["message"] = "Cache for this endpoint is still being built";
    }
    return errorResponse;
}

crow::response CacheManager::readinessBlockResponse(const CacheReadiness& readiness) {
    auto body = readinessBlockJson(readiness);
    crow::response response(503);
    response.set_header("Content-Type", "application/json");
    response.set_header("Retry-After", "5");
    response.write(body.dump());
    return response;
}

CacheManager::CacheReadinessSummary CacheManager::getReadinessSummary() const {
    std::lock_guard<std::mutex> lock(readiness_mutex_);
    CacheReadinessSummary summary;
    summary.total = static_cast<int>(readiness_.size());
    for (const auto& [key, readiness] : readiness_) {
        if (readiness.state == ReadinessState::Ready) {
            ++summary.ready;
            if (readiness.stale) {
                ++summary.stale;
                summary.stale_caches.push_back(readiness);
            }
        } else if (readiness.state == ReadinessState::Failed) {
            ++summary.failed;
            summary.failed_caches.push_back(readiness);
        } else if (readiness.state == ReadinessState::Unbuilt) {
            ++summary.unbuilt;
            summary.unbuilt_caches.push_back(readiness);
        } else {
            summary.pending_caches.push_back(readiness);
        }
    }
    return summary;
}

std::unique_lock<std::mutex> CacheManager::lockCatalogWrites() {
    std::unique_lock<std::mutex> lock(catalog_write_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        const auto waited_from = std::chrono::steady_clock::now();
        lock.lock();
        CROW_LOG_DEBUG << "Waited "
                       << std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - waited_from).count()
                       << " ms for another cache refresh/maintenance task on the catalog";
    }
    return lock;
}

bool CacheManager::enterRefresh(const CacheKey& key) {
    std::lock_guard<std::mutex> lock(inflight_mutex_);
    auto [it, inserted] = inflight_refreshes_.insert(key);
    return inserted;
}

void CacheManager::leaveRefresh(const CacheKey& key) {
    std::lock_guard<std::mutex> lock(inflight_mutex_);
    inflight_refreshes_.erase(key);
}

std::string CacheManager::renderRefreshTemplate(std::shared_ptr<ConfigManager> config_manager,
                                                const EndpointConfig& endpoint,
                                                std::map<std::string, std::string>& params,
                                                bool full) {
    const auto& cacheConfig = endpoint.cache;
    const auto& ducklakeConfig = config_manager->getDuckLakeConfig();
    const std::string catalog = ducklakeConfig.alias;
    const std::string schema = cacheConfig.schema.empty() ? "main" : cacheConfig.schema;
    const std::string table = cacheConfig.table;

    SnapshotInfo snapshot = fetchSnapshotInfo(catalog, schema, table);

    params.clear();
    params["cacheCatalog"] = catalog;
    params["cacheSchema"] = schema;
    params["cacheTable"] = table;

    params["cacheMode"] = determineCacheMode(cacheConfig);

    if (snapshot.current_snapshot_id) {
        params["cacheSnapshotId"] = *snapshot.current_snapshot_id;
    }
    if (snapshot.current_snapshot_committed_at) {
        params["cacheSnapshotTimestamp"] = *snapshot.current_snapshot_committed_at;
    }
    // The watermark for an incremental refresh is the LAST COMPLETED refresh,
    // which is the newest snapshot that exists right now - fetchSnapshotInfo
    // runs before this refresh writes anything, so index 0 is already it.
    //
    // These used to carry index 1, i.e. two refreshes back, while
    // CONFIG_REFERENCE documented previousSnapshotTimestamp as "Last refresh
    // timestamp" and its example used it as
    //     WHERE updated_at > TIMESTAMP '{{cache.previousSnapshotTimestamp}}'
    // So an append template re-read every row the previous refresh had already
    // appended - duplicates, silently - and a table's second-ever refresh had
    // no previous at all, so the incremental section did not render and it
    // reloaded everything.
    //
    // BEHAVIOUR CHANGE: incremental refreshes now load a narrower, correct
    // window. Merge mode was masking this - duplicates collapse on the primary
    // key - which is why it went unnoticed in append mode.
    if (snapshot.current_snapshot_id) {
        params["previousSnapshotId"] = *snapshot.current_snapshot_id;
    }
    if (snapshot.current_snapshot_committed_at) {
        params["previousSnapshotTimestamp"] = *snapshot.current_snapshot_committed_at;
    }

    // When a cursor is configured, the watermark comes from the DATA, not
    // from snapshot metadata.
    //
    // snapshot_time is the instant the previous refresh COMMITTED. That
    // refresh read the source at some earlier instant T_read. A source row
    // written at T with T_read < T <= T_commit was not read by refresh N, and
    // `WHERE updated_at > TIMESTAMP '<T_commit>'` excludes it from refresh
    // N+1 as well. On a continuously-written source every refresh
    // permanently drops the rows written during its own execution, and
    // reports success - invisible under both append and merge.
    //
    // max(cursor) over what is actually cached has no such window: rows above
    // it are exactly the rows not yet loaded. Re-reading a row at the
    // boundary is idempotent under merge and a bounded, visible duplicate
    // under append; losing it is neither.
    //
    // Falls back to the snapshot timestamp when the table is new or empty, so
    // a first refresh still renders its incremental section the same way.
    std::string watermark_dropped_reason;
    if (cacheConfig.hasCursor()) {
        const std::string watermark =
            fetchCursorWatermark(catalog, schema, table, cacheConfig.cursor->column,
                                 cacheConfig.cursor->type, &watermark_dropped_reason);
        if (!watermark.empty()) {
            params["previousSnapshotTimestamp"] = watermark;
        } else {
            // No watermark, and a cursor IS configured: ERASE it rather than
            // fall back to the snapshot commit timestamp.
            //
            // The two values do not share a type. With `cursor: {column: seq,
            // type: int}` over a created-but-empty cache table the fallback
            // renders `WHERE seq > 2026-09-23 10:00:00.123` - a binder error
            // on every subsequent refresh - and with a VARCHAR cursor it
            // silently compares against a date string. With a timestamp cursor
            // it renders `WHERE updated_at > '<table create time>'` and drops
            // every source row older than the table, forever: exactly the
            // silent loss this change exists to remove.
            //
            // Erasing makes `{{^cache.previousSnapshotTimestamp}}` render the
            // full-load branch, which is always correct and merely slower.
            // Only the TIMESTAMP. previousSnapshotId is still a valid
            // snapshot id and is independently documented for time travel -
            // erasing it removed an API a template may legitimately use while
            // taking the full-load branch for the watermark.
            params.erase("previousSnapshotTimestamp");
            CROW_LOG_INFO << "No cursor watermark for " << schema << "." << table
                          << " (" << cacheConfig.cursor->column
                          << "); this refresh loads the full source.";
        }
    }

    if (full) {
        // A forced full rebuild: no watermark, so `{{^cache.previousSnapshotTimestamp}}`
        // renders the template's full-load branch. Both the timestamp and the id go -
        // an incremental template may key on either. cache.snapshotId stays: it is the
        // table's current version, not a watermark.
        params.erase("previousSnapshotTimestamp");
        params.erase("previousSnapshotId");
    }

    if (cacheConfig.schedule) {
        params["cacheSchedule"] = cacheConfig.schedule.value();
    }
    if (cacheConfig.hasCursor()) {
        params["cursorColumn"] = cacheConfig.cursor->column;
        params["cursorType"] = cacheConfig.cursor->type;
    }
    if (cacheConfig.hasPrimaryKey()) {
        params["primaryKeys"] = joinStrings(cacheConfig.primary_keys, ",");
    }

    // Rendering consumes the cache.* entries (they move into the template
    // context); hand the caller the values it rendered with.
    const auto rendered_with = params;
    std::string rendered = db_adapter_->renderCacheTemplate(endpoint, cacheConfig, params);
    params = rendered_with;
    // After rendering: the reason is for the caller (expand --cache, the
    // refresh answer, the audit row), not a template variable. It used to be
    // in the server log only, so a cache silently reloaded in full every time.
    if (!watermark_dropped_reason.empty() && !full) {
        params[kWatermarkDroppedReasonKey] = watermark_dropped_reason;
    }
    return rendered;
}

void CacheManager::refreshDuckLakeCache(std::shared_ptr<ConfigManager> config_manager, const EndpointConfig& endpoint, std::map<std::string, std::string> params, const RefreshOptions& options) {
    const auto& cacheConfig = endpoint.cache;
    const auto& ducklakeConfig = config_manager->getDuckLakeConfig();
    const std::string catalog = ducklakeConfig.alias;
    const std::string schema = cacheConfig.schema.empty() ? "main" : cacheConfig.schema;
    const std::string table = cacheConfig.table;

    // Ensure cache schema exists before processing cache template
    ensureCacheSchemaExists(catalog, schema);

    std::string rendered = renderRefreshTemplate(config_manager, endpoint, params, options.full);
    std::string sync_type = determineCacheMode(cacheConfig);
    if (!cacheConfig.template_file) {
        // No template-file: the populate SQL is the endpoint's SERVING template,
        // which is normally a bare SELECT over the source. That used to run, build
        // nothing and record success; the bare-SELECT refusal then made such a
        // cache `failed` (503). It is materialised instead - a full refresh
        // whatever mode the cursor/primary-key imply, since a serving template has
        // no incremental branch. An explicit template-file is still refused a bare
        // SELECT (executeDuckLakeScript). The load-time warning says so.
        if (auto materialised = DatabaseManager::MaterialiseSelect(rendered, PopulateTarget{catalog, schema, table})) {
            rendered = std::move(*materialised);
            sync_type = "full";
        }
    }
    if (options.full) {
        CROW_LOG_INFO << "Full rebuild of " << schema << "." << table
                      << " requested: the watermark is cleared and the table is rebuilt from scratch";
    }

    // Timed around the populate statement only - what an operator tuning a
    // slow refresh can change - so the audit's duration_ms is comparable
    // across refreshes and does not include the snapshot/watermark lookups.
    SyncEventDetails details;
    // Rendered without a watermark means the template's full-load branch ran.
    details.full = options.full || sync_type == "full" || params.count("previousSnapshotTimestamp") == 0;
    // The table's newest snapshot BEFORE the populate: if it is still the newest
    // afterwards, this refresh committed nothing and has no snapshot of its own.
    const std::optional<std::string> snapshot_before =
        fetchSnapshotInfo(catalog, schema, table).current_snapshot_id;
    details.started_at = std::chrono::system_clock::now();
    try {
        if (shutdown_requested_.load()) {
            throw std::runtime_error("server is shutting down; refresh not started");
        }
        {
            // Published for the duration of the populate script, so a shutdown can
            // interrupt it instead of waiting minutes for a warm-up query to end.
            ActiveRefreshGuard active(*this);
            details.rows_affected = db_adapter_->executePopulateScript(
                rendered, PopulateTarget{catalog, schema, table}, options.full);
        }
        details.completed_at = std::chrono::system_clock::now();
        // Only a snapshot this refresh COMMITTED is reported. The id used to be
        // whatever the (name-only, so possibly another schema's) table lookup
        // matched first - every audit row and refresh answer carried the first
        // CTAS id. Unknown or unchanged is reported as null, never guessed.
        const SnapshotInfo after = fetchSnapshotInfo(catalog, schema, table);
        if (after.current_snapshot_id && after.current_snapshot_id != snapshot_before &&
            after.current_snapshot_id->rfind("snapshot_", 0) != 0) {
            details.snapshot_id = *after.current_snapshot_id;
        }
        std::string message = "Cache refreshed successfully";
        if (const auto it = params.find(kWatermarkDroppedReasonKey); it != params.end()) {
            details.warning = it->second;
            message += "; warning: " + it->second;
        }
        // Retention is PLANNED before the audit row is written, so a policy that
        // could not do what it says is in the audit message, not only in the log;
        // it EXPIRES after, so the newest audit snapshot (the one expiry keeps)
        // sees only this refresh's files and pins nothing older.
        const auto retention = effectiveRetention(config_manager, cacheConfig);
        if (retention.keep_last_snapshots || retention.max_snapshot_age) {
            applyRetention(catalog, schema, table, retention, [&](const std::string& warning) {
                if (!warning.empty()) {
                    message += "; retention: " + warning;
                }
                recordSyncEvent(config_manager, endpoint, sync_type, "success", message, details);
            });
        } else {
            recordSyncEvent(config_manager, endpoint, sync_type, "success", message, details);
        }
    } catch (const std::exception& ex) {
        details.completed_at = std::chrono::system_clock::now();
        // Scrubbed before it is RECORDED. A failed refresh's DuckDB error
        // quotes the rendered cache template, and this writes it into
        // cache.audit.sync_events - i.e. into DuckLake Parquet files, often
        // on object storage, where it is durable, backed up, time-travelled,
        // and readable through GET .../cache/audit and flapi_get_cache_audit.
        //
        // Every other copy of the rendered statement is scrubbed on its way
        // out; this was the one that persisted it AT REST, which is strictly
        // worse than the log leak that was fixed alongside it.
        const auto secrets = collectTemplateSecrets(config_manager.get(), endpoint, params);
        recordSyncEvent(config_manager, endpoint, sync_type, "error",
                        publicErrorMessage("Cache refresh failed", ex.what(), secrets), details);
        throw;
    }
}

CacheManager::RetentionOutcome CacheManager::applyRetention(const std::string& catalog,
                                                            const std::string& schema,
                                                            const std::string& table,
                                                            const CacheConfig::RetentionConfig& retention,
                                                            const std::function<void(const std::string&)>& before_expiry) {
    // The endpoint's own policy, else ducklake.retention.
    //
    // BOTH policies go through buildExpireSql, which expires explicit per-table
    // version ids. The age branch used to emit
    //   CALL ducklake_expire_snapshots(cat, older_than => <ts>)
    // which has no per-table form: it acts on the whole CATALOG, and every
    // cached endpoint shares one.
    RetentionOutcome outcome;
    std::string older_than_sql;
    if (retention.max_snapshot_age) {
        older_than_sql = "CAST(CURRENT_TIMESTAMP AS TIMESTAMP) - INTERVAL '" +
                         escapeSqlLiteral(retention.max_snapshot_age.value()) + "'";
    }
    ExpiryCandidates candidates;
    const std::string expireSql = buildExpireSql(
        catalog, schema, table, retention.keep_last_snapshots, older_than_sql, &candidates);
    if (candidates.shared > 0) {
        // A configured policy that cannot do what it says must be visible: this
        // was INFO, so a table whose snapshots were all shared grew silently.
        outcome.warning = std::to_string(candidates.shared) + " snapshot(s) of " + schema + "." + table +
                          " beyond the retention policy are shared with another table and were kept";
        CROW_LOG_WARNING << "Retention: " << outcome.warning;
    } else if (candidates.unresolved) {
        outcome.warning = "the table could not be resolved unambiguously in the catalog; nothing was expired";
        CROW_LOG_WARNING << "Retention for " << schema << "." << table << ": " << outcome.warning;
    }
    if (before_expiry) {
        before_expiry(outcome.warning);
    }
    if (expireSql.empty()) {
        return outcome;   // nothing this endpoint may expire yet
    }
    const auto bytes_before = referencedFileBytes(catalog);
    try {
        db_adapter_->executeDuckLakeQuery(expireSql, {});
        outcome.expired_snapshots = candidates.expirable.size();
        expireAuditSnapshots(catalog);   // they pin this table's expired files
        const auto bytes_after = referencedFileBytes(catalog);
        if (bytes_before && bytes_after && *bytes_before >= *bytes_after) {
            outcome.bytes_freed = *bytes_before - *bytes_after;
        }
        outcome.files_deleted = cleanupOldFiles(catalog);
    } catch (const std::exception& ex) {
        outcome.error = ex.what();
        CROW_LOG_WARNING << "Failed to expire DuckLake snapshots for " << schema << "." << table << ": " << ex.what();
    }
    return outcome;
}

std::optional<std::int64_t> CacheManager::referencedFileBytes(const std::string& catalog) {
    // Bytes of every data and delete file a snapshot still references. Expiry
    // removes the rows of the files it releases, so before minus after is what a
    // garbage collection freed (the files are deleted by cleanup_old_files next).
    try {
        const std::string meta = metadataCatalog(catalog);
        auto result = db_adapter_->executeDuckLakeQueryWithResult(
            "SELECT CAST((SELECT coalesce(sum(file_size_bytes), 0) FROM " + meta + ".ducklake_data_file) + "
            "(SELECT coalesce(sum(file_size_bytes), 0) FROM " + meta + ".ducklake_delete_file) AS BIGINT) AS bytes");
        auto rows = crow::json::load(result.data.dump());
        if (rows && rows.t() == crow::json::type::List && rows.size() == 1 && rows[0].has("bytes") &&
            rows[0]["bytes"].t() == crow::json::type::Number) {
            return static_cast<std::int64_t>(rows[0]["bytes"].d());
        }
    } catch (const std::exception& ex) {
        CROW_LOG_DEBUG << "Could not measure DuckLake file sizes: " << ex.what();
    }
    return std::nullopt;
}

CacheConfig::RetentionConfig CacheManager::effectiveRetention(std::shared_ptr<ConfigManager> config_manager,
                                                              const CacheConfig& cacheConfig) {
    // The endpoint's block wins as a whole: setting only `keep-last-snapshots` on an
    // endpoint must not inherit a global `max-snapshot-age` it did not ask for.
    if (cacheConfig.retention.keep_last_snapshots || cacheConfig.retention.max_snapshot_age) {
        return cacheConfig.retention;
    }
    CacheConfig::RetentionConfig global;
    const auto& ducklake = config_manager->getDuckLakeConfig().retention;
    global.keep_last_snapshots = ducklake.keep_last_snapshots;
    global.max_snapshot_age = ducklake.max_snapshot_age;
    return global;
}

std::size_t CacheManager::cleanupOldFiles(const std::string& catalog) {
    // Expiry only UNREFERENCES data files; until ducklake_cleanup_old_files runs they
    // stay on disk (or in the bucket) forever, so retention reclaimed no space. The
    // call is catalog-wide, which is safe: it deletes only files no remaining
    // snapshot of any table references. It answers one row per deleted file.
    try {
        auto result = db_adapter_->executeDuckLakeQueryWithResult(
            "CALL ducklake_cleanup_old_files('" + escapeSqlLiteral(catalog) + "', cleanup_all => true)");
        auto rows = crow::json::load(result.data.dump());
        if (rows && rows.t() == crow::json::type::List) {
            return rows.size();
        }
    } catch (const std::exception& ex) {
        CROW_LOG_WARNING << "Failed to delete expired DuckLake data files in catalog '" << catalog
                         << "': " << ex.what();
    }
    return 0;
}

bool CacheManager::isPlausibleWatermark(const std::string& value,
                                        const std::string& cursor_type) {
    if (value.empty()) {
        return false;
    }

    std::string type;
    type.reserve(cursor_type.size());
    for (const char c : cursor_type) {
        type += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    const auto all_of = [&value](const char* allowed) {
        return value.find_first_not_of(allowed) == std::string::npos;
    };

    if (type == "timestamp" || type == "datetime" || type == "date" ||
        type == "time" || type == "timestamptz") {
        return all_of("0123456789:-+. TZ");
    }
    if (type == "int" || type == "integer" || type == "bigint" ||
        type == "smallint" || type == "hugeint" || type == "long" ||
        type == "double" || type == "decimal" || type == "numeric" ||
        type == "float") {
        return all_of("0123456789+-.eE");
    }

    // Anything else, `string` included: no character that can end a literal,
    // start a statement, or open a comment.
    if (value.find_first_of("'\"`;\n\r\\") != std::string::npos) {
        return false;
    }
    return value.find("--") == std::string::npos &&
           value.find("/*") == std::string::npos;
}

std::string CacheManager::fetchCursorWatermark(const std::string& catalog,
                                               const std::string& schema,
                                               const std::string& table,
                                               const std::string& cursor_column,
                                               const std::string& cursor_type,
                                               std::string* dropped_reason) {
    if (cursor_column.empty()) {
        return {};
    }
    try {
        const std::string query =
            "SELECT CAST(max(" + quoteIdentifier(cursor_column) + ") AS VARCHAR) AS watermark FROM " +
            quoteIdentifier(catalog) + "." + quoteIdentifier(schema) + "." +
            quoteIdentifier(table);
        auto result = db_adapter_->executeDuckLakeQueryWithResult(query);
        auto rows = crow::json::load(result.data.dump());
        if (rows && rows.t() == crow::json::type::List && rows.size() > 0 &&
            rows[0].has("watermark") &&
            rows[0]["watermark"].t() == crow::json::type::String) {
            const std::string watermark = rows[0]["watermark"].s();
            if (!isPlausibleWatermark(watermark, cursor_type)) {
                // Dropped, not escaped. The caller then erases the watermark
                // and the refresh takes its full-load branch, which is always
                // correct and merely slower - see isPlausibleWatermark.
                const std::string article =
                    (!cursor_type.empty() && std::string("aeiouAEIOU").find(cursor_type[0]) != std::string::npos)
                        ? "an " : "a ";
                const std::string reason =
                    "the cursor watermark read from " + schema + "." + table + " (" + cursor_column +
                    ") does not look like " + article + cursor_type +
                    " (cursor.type); it was not interpolated and this refresh loaded the full source - "
                    "set cursor.type to the column's type";
                CROW_LOG_WARNING << "The " << reason.substr(4);
                if (dropped_reason != nullptr) {
                    *dropped_reason = reason;
                }
                return {};
            }

            // Returned RAW.
            //
            // It used to be pre-escaped here, which is wrong twice over: the
            // docs say the value is rendered raw, and the documented
            // double-brace form `{{cache.previousSnapshotTimestamp}}`
            // HTML-escapes what it is given, so a pre-escaped `ab''c` renders
            // as `ab&#39;&#39;c`. Escaping belongs at interpolation, where
            // the surrounding quoting is known - which is what the template
            // author chooses with `{{{ }}}` versus `{{ }}`.
            //
            // A quote in a cursor value is vanishingly rare (cursors are
            // timestamps, sequences or dates) and mangling every ordinary
            // value to guard it is the worse trade.
            return watermark;
        }
    } catch (const std::exception& ex) {
        // Table not created yet, or the cursor column is not in it. The
        // snapshot timestamp remains the fallback.
        CROW_LOG_DEBUG << "No cursor watermark for " << schema << "." << table
                       << " (" << cursor_column << "): " << ex.what();
    }
    return {};
}

std::string CacheManager::escapeSqlLiteral(const std::string& value) {
    // Doubling is the whole of SQL string escaping. These literals are built
    // by us, but the values inside them - a cursor watermark, a table name, a
    // configured interval - come from upstream data or user config.
    std::string out;
    out.reserve(value.size());
    for (const char c : value) {
        if (c == '\'') {
            out += "''";
        } else {
            out += c;
        }
    }
    return out;
}

std::string CacheManager::quoteIdentifier(const std::string& name) {
    std::string out = "\"";
    for (const char c : name) {
        // Doubling, not tripling: appending "" and then falling through to
        // append the character again produced three quotes and invalid SQL
        // for any legitimately quoted identifier containing one.
        if (c == '"') {
            out += "\"\"";
        } else {
            out += c;
        }
    }
    out += '"';
    return out;
}

std::optional<std::vector<std::string>> CacheManager::liveTableChangeKeys(
        const std::string& catalog) {
    std::vector<std::string> keys;
    try {
        // BOTH forms. `changes` names a table by its numeric id for row
        // changes and by `schema.table` for creates, so collecting only ids
        // misses the qualified name - and a transaction that creates this
        // table alongside another would then look exclusive, because the
        // other table's create key was never in the foreign set.
        //
        // ducklake_table_info exposes schema_id rather than a schema name, so
        // the qualified names come from duckdb_tables() over the attached
        // catalog.
        const std::string ids_query =
            "SELECT CAST(table_id AS VARCHAR) AS table_id "
            "FROM ducklake_table_info('" + escapeSqlLiteral(catalog) + "')";
        auto ids = db_adapter_->executeDuckLakeQueryWithResult(ids_query);
        auto id_rows = crow::json::load(ids.data.dump());
        if (!(id_rows && id_rows.t() == crow::json::type::List)) {
            return std::nullopt;
        }
        for (std::size_t i = 0; i < id_rows.size(); ++i) {
            // A row we cannot read is a table we cannot account for, and an
            // unaccounted-for table is exactly what the exclusivity filter
            // needs to know about. Skipping it silently reported success on a
            // partial listing - the same fail-open, one level down.
            if (!(id_rows[i].has("table_id") &&
                  id_rows[i]["table_id"].t() == crow::json::type::String)) {
                CROW_LOG_WARNING << "ducklake_table_info returned a row without a "
                                    "readable table_id; retention cannot prove a "
                                    "snapshot is unshared and will expire nothing.";
                return std::nullopt;
            }
            keys.push_back(id_rows[i]["table_id"].s());
        }

        const std::string names_query =
            "SELECT schema_name, table_name FROM duckdb_tables() "
            "WHERE database_name = '" + escapeSqlLiteral(catalog) + "'";
        auto names = db_adapter_->executeDuckLakeQueryWithResult(names_query);
        auto name_rows = crow::json::load(names.data.dump());
        if (!(name_rows && name_rows.t() == crow::json::type::List)) {
            return std::nullopt;
        }
        for (std::size_t i = 0; i < name_rows.size(); ++i) {
            if (!(name_rows[i].has("schema_name") && name_rows[i].has("table_name") &&
                  name_rows[i]["schema_name"].t() == crow::json::type::String &&
                  name_rows[i]["table_name"].t() == crow::json::type::String)) {
                CROW_LOG_WARNING << "duckdb_tables() returned a row without a readable "
                                    "schema/table name; retention will expire nothing.";
                return std::nullopt;
            }
            // Both the bare and the quoted form DuckLake writes into
            // `tables_created` - see tableNameKeys.
            for (auto& name_key : tableNameKeys(name_rows[i]["schema_name"].s(),
                                                name_rows[i]["table_name"].s())) {
                keys.push_back(std::move(name_key));
            }
        }
    } catch (const std::exception& ex) {
        CROW_LOG_WARNING << "Could not list the tables in DuckLake catalog '" << catalog
                         << "', so snapshot retention cannot prove a snapshot is not "
                            "shared with another table and will expire nothing this "
                            "cycle: " << ex.what();
        return std::nullopt;
    }
    return keys;
}

std::string CacheManager::metadataCatalog(const std::string& catalog) {
    // DuckLake attaches its metadata database under this fixed name, whatever the
    // backend (DuckDB file, Postgres, SQLite, MySQL).
    return quoteIdentifier("__ducklake_metadata_" + catalog);
}

std::vector<std::string> CacheManager::tableNameKeys(const std::string& schema, const std::string& table) {
    // `tables_created` names a table as DuckLake WRITES it: each part through
    // KeywordHelper::WriteOptionallyQuoted. So the default schema `cache` - a
    // keyword - is recorded as '"cache".orders', and matching only the bare
    // 'cache.orders' made every full-refresh snapshot of every default-schema
    // cache invisible to retention: nothing ever expired and the disk grew
    // forever. Measured on DuckDB 1.5.5. The bare form is kept as well, in case
    // a DuckLake version writes it unquoted.
    std::vector<std::string> keys;
    keys.push_back(schema + "." + table);
    const std::string quoted = ::duckdb::KeywordHelper::WriteOptionallyQuoted(schema) + "." +
                               ::duckdb::KeywordHelper::WriteOptionallyQuoted(table);
    if (quoted != keys.front()) {
        keys.push_back(quoted);
    }
    return keys;
}

std::vector<std::string> CacheManager::tableChangeKeys(const std::string& catalog,
                                                       const std::string& schema,
                                                       const std::string& table,
                                                       bool strict) {
    // `changes` is a MAP(VARCHAR, VARCHAR[]) that names tables by id for row
    // changes and by `schema.table` for creates, so both forms count.
    std::vector<std::string> keys = tableNameKeys(schema, table);

    // Every table id this schema.table has had, LIVE AND DROPPED, resolved
    // through the schema NAME.
    //
    // A full refresh is a CREATE OR REPLACE, and a `--full` rebuild of a merge
    // cache a DROP + CREATE: each gives the table a new id. Matching only the
    // live id left every earlier incarnation's snapshots - `tables_inserted_into`
    // = [old id], nothing else - unmatched, so they were never expirable and each
    // rebuild leaked the previous base files (~240 MB each, measured). The
    // metadata catalog's ducklake_table keeps one row per incarnation (until
    // expiry removes it), joined to ducklake_schema for the name that
    // ducklake_table_info() does not expose.
    try {
        const std::string meta = metadataCatalog(catalog);
        const std::string query =
            "SELECT DISTINCT CAST(t.table_id AS VARCHAR) AS table_id FROM " + meta +
            ".ducklake_table t JOIN " + meta + ".ducklake_schema s ON t.schema_id = s.schema_id "
            "WHERE s.schema_name = '" + escapeSqlLiteral(schema) + "' AND t.table_name = '" +
            escapeSqlLiteral(table) + "' ORDER BY 1";
        auto result = db_adapter_->executeDuckLakeQueryWithResult(query);
        auto rows = crow::json::load(result.data.dump());
        if (rows && rows.t() == crow::json::type::List) {
            for (std::size_t i = 0; i < rows.size(); ++i) {
                if (!(rows[i].has("table_id") && rows[i]["table_id"].t() == crow::json::type::String)) {
                    if (strict) {
                        CROW_LOG_WARNING << "DuckLake metadata returned a table row without a readable "
                                            "table_id for " << schema << "." << table
                                         << "; retention will expire nothing this cycle.";
                        return {};
                    }
                    continue;
                }
                keys.push_back(rows[i]["table_id"].s());
            }
            return keys;
        }
    } catch (const std::exception& ex) {
        CROW_LOG_DEBUG << "DuckLake metadata tables not readable for " << schema << "." << table
                       << " (" << ex.what() << "); resolving the live table id only";
    }

    // Fallback: the live table id by name only.
    try {
        // ducklake_table_info() carries table_name and schema_id but NOT a
        // schema NAME, so `WHERE table_name = 'orders' LIMIT 1` over a catalog
        // holding s1.orders and s2.orders returns an arbitrary one of them.
        // Verified on DuckDB 1.5.5: two rows differing only in schema_id.
        //
        // An ambiguous lookup on the destructive path resolves to NOTHING, and
        // callers that would destroy data do nothing at all.
        const std::string query =
            "SELECT CAST(table_id AS VARCHAR) AS table_id FROM ducklake_table_info('" +
            escapeSqlLiteral(catalog) + "') WHERE table_name = '" +
            escapeSqlLiteral(table) + "'";
        auto result = db_adapter_->executeDuckLakeQueryWithResult(query);
        auto rows = crow::json::load(result.data.dump());
        if (rows && rows.t() == crow::json::type::List) {
            if (rows.size() > 1) {
                if (strict) {
                    CROW_LOG_WARNING
                        << "DuckLake catalog '" << catalog << "' holds " << rows.size()
                        << " tables named '" << table << "' in different schemas, and "
                           "its metadata tables could not be read to tell them apart. "
                           "Snapshot retention for " << schema << "." << table
                        << " is disabled rather than risk expiring another table's "
                           "snapshots.";
                    return {};
                }
                // Ambiguous: the name keys alone. A guessed id here used to be
                // reported as the refresh's snapshot_id (always the first table's).
                return keys;
            }
            if (rows.size() == 1 && rows[0].has("table_id") &&
                rows[0]["table_id"].t() == crow::json::type::String) {
                keys.push_back(rows[0]["table_id"].s());
            }
        }
    } catch (const std::exception& ex) {
        // Table not in the catalog yet on a first refresh; the name form alone
        // is the right answer then.
        CROW_LOG_DEBUG << "Could not resolve DuckLake table id for " << schema << "." << table
                       << ": " << ex.what();
    }
    return keys;
}

std::string CacheManager::tableSnapshotPredicate(const std::vector<std::string>& keys) {
    if (keys.empty()) {
        return "false";
    }
    std::ostringstream out;
    out << "(";
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (i > 0) {
            out << " OR ";
        }
        // Escaped here too. The exclusivity expression built from this same
        // vector escapes; this one did not, so a cache `table:` containing a
        // quote broke the retention query AND fetchSnapshotInfo - silently
        // disabling both retention and the incremental watermark.
        out << "list_contains(flatten(map_values(changes)), '"
            << escapeSqlLiteral(keys[i]) << "')";
    }
    out << ")";
    return out.str();
}

CacheManager::ExpiryCandidates CacheManager::expirableSnapshotIds(
        const std::string& catalog,
        const std::string& schema,
        const std::string& table,
        std::optional<std::size_t> keep_last,
        const std::string& older_than_sql) {
    ExpiryCandidates out;
    try {
        // Every cached endpoint shares ONE DuckLake catalog, so a
        // catalog-wide snapshot list is every endpoint's history. Both expiry
        // policies used to act catalog-wide - count-based via a bare
        // `SELECT snapshot_id FROM ducklake_snapshots(cat)`, and age-based and
        // manual GC via `ducklake_expire_snapshots(cat, older_than => ...)`,
        // which has no per-table form at all. Retention is configured PER
        // ENDPOINT, so any of them destroyed every other endpoint's
        // time-travel history and its incremental watermark.
        //
        // Two restrictions, both needed:
        //   1. only snapshots that touched THIS table are candidates;
        //   2. of those, only ones that touched NOTHING ELSE are expired.
        //
        // (2) is the conservative half. A snapshot can carry changes for
        // several tables, and expiring it discards all of them - so a shared
        // snapshot is left alone and this endpoint simply keeps more history
        // than asked. Retaining too much is recoverable; deleting another
        // endpoint's data is not.
        const auto keys = tableChangeKeys(catalog, schema, table, /*strict=*/true);
        if (keys.empty()) {
            // The table could not be identified unambiguously. Expiring on a
            // guess is destructive, so expire nothing - see tableChangeKeys.
            out.unresolved = true;
            return out;
        }

        // "Touched nothing but this table": every entry in the snapshot's
        // change list is one of this table's keys. Built from resolved
        // literals - a subquery inside the lambda is a binder error.
        //
        // Measured shapes of `changes` on DuckDB 1.5.5 / DuckLake:
        //   CREATE SCHEMA -> {schemas_created=[s1]}
        //   CREATE TABLE  -> {tables_created=[s1.orders], inlined_insert=[3]}
        //   INSERT        -> {inlined_insert=[3]}
        // so creates name the table as `schema.table` and row changes by its
        // numeric id; both are in `keys`, and a schema-create snapshot is not
        // a candidate at all because it names neither.
        // "Touched no OTHER LIVE table." A change key that names neither this
        // table nor any table currently in the catalog belongs to a dropped
        // one - and a dropped table's snapshots are nobody's data to protect.
        //
        // Matching on "not one of our keys" alone made a dropped-and-recreated
        // cache table permanently unexpirable: its older snapshots still carry
        // the previous incarnation's table id, so every refresh reported
        // "N snapshots are shared with another cached table", which was false
        // and masked genuine sharing. Verified on DuckDB 1.5.5 that
        // ducklake_table_info lists only the live table after DROP+CREATE
        // while ducklake_snapshots keeps the old id.
        const auto live = liveTableChangeKeys(catalog);
        if (!live) {
            // Could not establish what else is live, so nothing can be shown
            // to be unshared. Expire nothing rather than guess: the warning
            // is already logged above.
            return out;
        }

        std::vector<std::string> foreign_keys;
        for (const auto& live_key : *live) {
            if (std::find(keys.begin(), keys.end(), live_key) == keys.end()) {
                foreign_keys.push_back(live_key);
            }
        }

        std::ostringstream others;
        if (foreign_keys.empty()) {
            others << "true";   // no other live table exists to share with
        } else {
            others << "len(list_filter(flatten(map_values(changes)), x -> ";
            for (std::size_t i = 0; i < foreign_keys.size(); ++i) {
                if (i > 0) {
                    others << " OR ";
                }
                others << "x = '" << escapeSqlLiteral(foreign_keys[i]) << "'";
            }
            others << ")) = 0";
        }

        // Both predicates are SELECTed, not filtered on, so one query answers
        // every part of the decision:
        //   `exclusive` - this snapshot touched nothing but this table, so
        //                 expiring it cannot discard another table's data;
        //   `aged`      - it is older than `max-snapshot-age`.
        //
        // The age predicate used to be in the WHERE clause, which ranked
        // `keep_last` over the ALREADY-AGED set: `keep-last-snapshots: 3` with
        // `max-snapshot-age: 7d` kept "the newest 3 stale ones, plus
        // everything newer than 7d" rather than 3. The examples ship exactly
        // that pair. Ranking over the table's whole history and applying both
        // conditions to each row is what the two keys read like.
        std::string aged = older_than_sql.empty()
                               ? std::string("true")
                               : "snapshot_time < " + older_than_sql;
        const std::string query =
            "SELECT snapshot_id, (" + others.str() + ") AS exclusive, (" + aged + ") AS aged "
            "FROM ducklake_snapshots('" + escapeSqlLiteral(catalog) + "') "
            "WHERE " + tableSnapshotPredicate(keys) +
            " ORDER BY snapshot_id DESC";

        auto result = db_adapter_->executeDuckLakeQueryWithResult(query);
        auto rows = crow::json::load(result.data.dump());
        if (!(rows && rows.t() == crow::json::type::List)) {
            return out;
        }

        // ALWAYS retain at least the newest snapshot of this table.
        //
        // With `max-snapshot-age` alone, skip was 0 and every match - the
        // current snapshot included - was a candidate. A table refreshed
        // weekly under `max-snapshot-age: 1d` would either have its expiry
        // rejected wholesale (silently, at WARNING) or lose the snapshot
        // fetchSnapshotInfo reads its incremental watermark from. The
        // catalog-wide version had the same shape; the rewrite is the moment
        // to give it a floor.
        const std::size_t skip = std::max<std::size_t>(1, keep_last.value_or(0));
        for (std::size_t i = skip; i < rows.size(); ++i) {
            const auto& row = rows[i];
            if (!(row.has("snapshot_id") &&
                  row["snapshot_id"].t() == crow::json::type::Number)) {
                continue;
            }
            // Absent means "cannot tell", NOT "old enough". This read
            // `!row.has("aged") || ...`, so a result without the column made
            // every snapshot eligible - the same fail-open shape as the
            // live-table listing, one loop away from it, on the same
            // destructive path.
            if (!row.has("aged")) {
                CROW_LOG_WARNING << "DuckLake returned no `aged` column for "
                                 << schema << "." << table
                                 << "; retention cannot tell which snapshots are old "
                                    "enough and will expire nothing this cycle.";
                return {};
            }
            if (row["aged"].t() != crow::json::type::True) {
                continue;   // newer than max-snapshot-age: retained
            }
            const bool exclusive = row.has("exclusive") &&
                                   row["exclusive"].t() == crow::json::type::True;
            if (exclusive) {
                out.expirable.push_back(static_cast<std::int64_t>(row["snapshot_id"].d()));
            } else {
                ++out.shared;
            }
        }
    } catch (const std::exception& ex) {
        CROW_LOG_WARNING << "Could not list DuckLake snapshots for retention on "
                         << schema << "." << table << ": " << ex.what();
        return {};
    }
    return out;
}

std::string CacheManager::buildExpireSql(const std::string& catalog,
                                         const std::string& schema,
                                         const std::string& table,
                                         std::optional<std::size_t> keep_last,
                                         const std::string& older_than_sql,
                                         ExpiryCandidates* report) {
    // `keep-last-snapshots: 0` is not a retention policy, but it must not
    // disable an accompanying `max-snapshot-age` either - which is what
    // returning here did when both keys were set.
    if (keep_last.has_value() && *keep_last == 0) {
        if (older_than_sql.empty()) {
            return {};
        }
        keep_last.reset();
    }

    const auto candidates =
        expirableSnapshotIds(catalog, schema, table, keep_last, older_than_sql);
    if (report != nullptr) {
        *report = candidates;
    }
    const auto& expire_ids = candidates.expirable;
    if (expire_ids.empty()) {
        // "Nothing old enough" and "nothing expirable" look identical from
        // outside; the caller (applyRetention) reports `shared` as a warning.
        return {};
    }

    std::ostringstream sql;
    sql << "CALL ducklake_expire_snapshots('" << escapeSqlLiteral(catalog) << "', versions => [";
    for (std::size_t i = 0; i < expire_ids.size(); ++i) {
        if (i > 0) {
            sql << ", ";
        }
        sql << expire_ids[i];
    }
    sql << "])";
    return sql.str();
}

std::string CacheManager::determineCacheMode(const CacheConfig& cacheConfig) {
    if (!cacheConfig.hasCursor()) {
        return "full";
    }
    return cacheConfig.hasPrimaryKey() ? "merge" : "append";
}

std::string CacheManager::joinStrings(const std::vector<std::string>& values, const std::string& delimiter) {
    if (values.empty()) {
        return {};
    }
    std::ostringstream joined;
    for (std::size_t idx = 0; idx < values.size(); ++idx) {
        if (idx > 0) {
            joined << delimiter;
        }
        joined << values[idx];
    }
    return joined.str();
}

CacheManager::SnapshotInfo CacheManager::fetchSnapshotInfo(const std::string& catalog, const std::string& schema, const std::string& table) {
    SnapshotInfo info;

    try {
        std::map<std::string, std::string> params;
        params["catalog"] = catalog;
        params["schema"] = schema;
        params["table"] = table;

        // Snapshots THAT TOUCHED THIS TABLE, newest first.
        //
        // This used to be the newest two snapshots in the CATALOG, which is a
        // silent data-loss bug as soon as more than one endpoint is cached:
        // every cache shares one DuckLake catalog, so another table's refresh
        // becomes this table's "previous snapshot". Its timestamp is newer
        // than this table's real previous refresh, and
        // {{cache.previousSnapshotTimestamp}} drives the incremental WHERE
        // clause - so every row changed between this table's last refresh and
        // the other table's is skipped. The refresh reports success.
        //
        // Demonstrated on a real DuckLake catalog with two cached tables a and
        // b, refreshed a, b, a: the snapshots touching `a` are [4, 2], but the
        // catalog-wide query returned [4, 3] - and 3 is b's.
        //
        // `changes` is a MAP(VARCHAR, VARCHAR[]) that names tables by id for
        // row changes and by `schema.table` for creates, so both forms are
        // matched. A NULL table id (table not in the catalog yet) makes
        // list_contains NULL, which the OR handles.
        std::string snapshotsQuery =
            "SELECT snapshot_id, snapshot_time FROM ducklake_snapshots('" + escapeSqlLiteral(catalog) + "') "
            "WHERE " + tableSnapshotPredicate(tableChangeKeys(catalog, schema, table, /*strict=*/false)) + " "
            "ORDER BY snapshot_id DESC LIMIT 2";
        try {
            auto snapshots = db_adapter_->executeDuckLakeQueryWithResult(snapshotsQuery);
            auto snapshotsJson = crow::json::load(snapshots.data.dump());

            if (!(snapshotsJson && snapshotsJson.t() == crow::json::type::List && snapshotsJson.size() > 0)) {
                return info;
            }

            // Current snapshot is the highest version (first row)
            const auto& currentRow = snapshotsJson[0];
            if (currentRow.has("snapshot_id") && currentRow["snapshot_id"].t() == crow::json::type::Number) {
                info.current_snapshot_id = std::to_string(static_cast<int64_t>(currentRow["snapshot_id"].d()));
            }
            if (currentRow.has("snapshot_time") && currentRow["snapshot_time"].t() == crow::json::type::String) {
                info.current_snapshot_committed_at = currentRow["snapshot_time"].s();
            }

            // Previous snapshot is the second highest version (if available)
            if (snapshotsJson.size() > 1) {
                const auto& previousRow = snapshotsJson[1];
                if (previousRow.has("snapshot_id") && previousRow["snapshot_id"].t() == crow::json::type::Number) {
                    info.previous_snapshot_id = std::to_string(static_cast<int64_t>(previousRow["snapshot_id"].d()));
                }
                if (previousRow.has("snapshot_time") && previousRow["snapshot_time"].t() == crow::json::type::String) {
                    info.previous_snapshot_committed_at = previousRow["snapshot_time"].s();
                }
            }
        } catch (const std::exception& ex) {
            CROW_LOG_DEBUG << "DuckLake snapshots function not available, using fallback: " << ex.what();
            // Use current timestamp as fallback
            info.current_snapshot_id = "snapshot_" + std::to_string(std::time(nullptr));
            info.current_snapshot_committed_at = "now";
        }

    } catch (const std::exception& ex) {
        CROW_LOG_WARNING << "Failed to fetch snapshot info for " << schema << "." << table << ": " << ex.what();
    }

    return info;
}

void CacheManager::addQueryCacheParamsIfNecessary(std::shared_ptr<ConfigManager> config_manager, const EndpointConfig& endpoint, std::map<std::string, std::string>& params) {
    auto &cacheConfig = endpoint.cache;
    if (!cacheConfig.enabled || cacheConfig.table.empty()) {
        return;
    }

    std::string schema = cacheConfig.schema.empty() ? config_manager->getCacheSchema() : cacheConfig.schema;
    // ASSIGN, never emplace: emplace left a request-supplied `?cacheTable=...` in place, so a
    // caller could point a cached endpoint's template at any table (#192). These are
    // server-owned.
    params["cacheCatalog"] = config_manager->getDuckLakeConfig().alias;
    params["cacheSchema"] = schema;
    params["cacheTable"] = cacheConfig.table;
}

CacheManager::GarbageCollectionResult CacheManager::performGarbageCollection(std::shared_ptr<ConfigManager> config_manager, const EndpointConfig& endpoint, const std::vector<std::string> previousTableNames) {
    GarbageCollectionResult result;
    const auto& cacheConfig = endpoint.cache;
    const auto retention = effectiveRetention(config_manager, cacheConfig);
    if (!retention.keep_last_snapshots && !retention.max_snapshot_age) {
        // It used to answer "Garbage collection completed" here, having done
        // nothing: GC obeys the retention policy, and there is none.
        result.status = "no_policy";
        result.message = "no retention policy configured; nothing expired "
                         "(set cache.retention or ducklake.retention)";
        return result;
    }

    const std::string catalog = config_manager->getDuckLakeConfig().alias;
    const std::string schema = cacheConfig.schema.empty() ? "main" : cacheConfig.schema;
    const std::string table = cacheConfig.table;
    auto catalog_lock = lockCatalogWrites();

    // Manual GC obeys the endpoint's OWN retention policy, per table - see
    // applyRetention. It used to be a catalog-wide `older_than` expiry against
    // a HARDCODED one-day cutoff.
    SyncEventDetails details;
    details.started_at = std::chrono::system_clock::now();
    const auto outcome = applyRetention(catalog, schema, table, retention);
    details.completed_at = std::chrono::system_clock::now();
    result.expired_snapshots = outcome.expired_snapshots;
    result.files_deleted = outcome.files_deleted;
    result.bytes_freed = outcome.bytes_freed;
    result.warning = outcome.warning;

    if (!outcome.error.empty()) {
        result.status = "error";
        std::map<std::string, std::string> no_params;
        const auto secrets = collectTemplateSecrets(config_manager.get(), endpoint, no_params);
        result.message = publicErrorMessage("Garbage collection failed", outcome.error, secrets);
        recordSyncEvent(config_manager, endpoint, "garbage_collection", "error", result.message, details);
        return result;
    }
    result.status = outcome.expired_snapshots > 0 ? "expired" : "nothing_to_expire";
    std::ostringstream message;
    if (outcome.expired_snapshots > 0) {
        message << "Expired " << outcome.expired_snapshots << " snapshot(s), deleted "
                << outcome.files_deleted << " file(s), freed " << outcome.bytes_freed << " bytes";
    } else {
        message << "No snapshots eligible for expiry";
    }
    if (!outcome.warning.empty()) {
        message << "; warning: " << outcome.warning;
    }
    result.message = message.str();
    recordSyncEvent(config_manager, endpoint, "garbage_collection", "success", result.message, details);
    return result;
}

CacheManager::GarbageCollectionResult CacheManager::dropObsoleteCacheTable(
        std::shared_ptr<ConfigManager> config_manager, const std::string& schema, const std::string& table) {
    GarbageCollectionResult result;
    const std::string catalog = config_manager->getDuckLakeConfig().alias;
    auto catalog_lock = lockCatalogWrites();
    if (!cacheTableExists(catalog, schema, table)) {
        result.status = "not_found";
        result.message = "no table " + schema + "." + table + " in DuckLake catalog '" + catalog + "'";
        return result;
    }
    // A table left behind by a changed `schema:` (or `table:`) is nobody's to
    // expire: no endpoint's retention matches it, so its snapshots and files
    // stayed forever.
    //
    // The table's first snapshot opens the window in which its files were live.
    std::optional<std::int64_t> first_snapshot;
    try {
        auto first = db_adapter_->executeDuckLakeQueryWithResult(
            "SELECT min(snapshot_id) AS id FROM ducklake_snapshots('" + escapeSqlLiteral(catalog) + "') WHERE " +
            tableSnapshotPredicate(tableChangeKeys(catalog, schema, table, /*strict=*/true)));
        auto rows = crow::json::load(first.data.dump());
        if (rows && rows.t() == crow::json::type::List && rows.size() == 1 && rows[0].has("id") &&
            rows[0]["id"].t() == crow::json::type::Number) {
            first_snapshot = static_cast<std::int64_t>(rows[0]["id"].d());
        }
    } catch (const std::exception& ex) {
        CROW_LOG_DEBUG << "No first snapshot for " << schema << "." << table << ": " << ex.what();
    }
    db_adapter_->executeDuckLakeQuery("DROP TABLE " + quoteIdentifier(catalog) + "." + quoteIdentifier(schema) +
                                          "." + quoteIdentifier(table), {});
    CROW_LOG_INFO << "Dropped obsolete cache table " << schema << "." << table;
    result.status = "dropped";

    // Recorded BEFORE the expiry, filed under no endpoint like compaction: its
    // snapshot is then the newest, so the DROP's own snapshot (which could see
    // every other table's files of that moment) is no longer the catalog's
    // latest and can be expired too.
    EndpointConfig maintenance;
    recordSyncEvent(config_manager, maintenance, "drop_table", "success",
                    "Dropped obsolete cache table " + schema + "." + table);

    // A file stays on disk while ANY snapshot that could see it survives. Every
    // snapshot since the table's first that touched no LIVE table - the dropped
    // table's own history, the DROP, a `CREATE SCHEMA` for the new `schema:` - is
    // nobody's history any more. Snapshots of live tables are left to those
    // tables' own retention (they release this table's files as they expire).
    const auto live = liveTableChangeKeys(catalog);
    if (first_snapshot && live) {
        std::ostringstream touches_live;
        if (live->empty()) {
            touches_live << "false";
        } else {
            touches_live << "len(list_filter(flatten(map_values(changes)), x -> ";
            for (std::size_t i = 0; i < live->size(); ++i) {
                touches_live << (i > 0 ? " OR " : "") << "x = '" << escapeSqlLiteral((*live)[i]) << "'";
            }
            touches_live << ")) > 0";
        }
        try {
            auto window = db_adapter_->executeDuckLakeQueryWithResult(
                "SELECT snapshot_id FROM ducklake_snapshots('" + escapeSqlLiteral(catalog) + "') "
                "WHERE snapshot_id >= " + std::to_string(*first_snapshot) +
                " AND snapshot_id < (SELECT max(snapshot_id) FROM ducklake_snapshots('" +
                escapeSqlLiteral(catalog) + "')) AND NOT (" + touches_live.str() + ") ORDER BY snapshot_id");
            auto rows = crow::json::load(window.data.dump());
            std::vector<std::int64_t> ids;
            if (rows && rows.t() == crow::json::type::List) {
                for (std::size_t i = 0; i < rows.size(); ++i) {
                    if (rows[i].has("snapshot_id") && rows[i]["snapshot_id"].t() == crow::json::type::Number) {
                        ids.push_back(static_cast<std::int64_t>(rows[i]["snapshot_id"].d()));
                    }
                }
            }
            const auto bytes_before = referencedFileBytes(catalog);
            if (!ids.empty()) {
                std::ostringstream sql;
                sql << "CALL ducklake_expire_snapshots('" << escapeSqlLiteral(catalog) << "', versions => [";
                for (std::size_t i = 0; i < ids.size(); ++i) {
                    sql << (i > 0 ? ", " : "") << ids[i];
                }
                sql << "])";
                db_adapter_->executeDuckLakeQuery(sql.str(), {});
                result.expired_snapshots = ids.size();
            }
            expireAuditSnapshots(catalog);   // audit snapshots of that window pin the files too

            // What is left in the window touched a live table, so it is that
            // table's history and its retention decides. Applied NOW rather than
            // at that table's next refresh, so the dropped table's files go as
            // soon as the policy allows; whatever the policy still keeps is named.
            std::set<std::pair<std::string, std::string>> seen;
            for (const auto& endpoint : *config_manager->getEndpoints()) {
                if (!endpoint.cache.enabled || endpoint.cache.table.empty()) {
                    continue;
                }
                const std::string ep_schema = endpoint.cache.schema.empty() ? "main" : endpoint.cache.schema;
                if (!seen.insert({ep_schema, endpoint.cache.table}).second) {
                    continue;
                }
                const auto keys = tableChangeKeys(catalog, ep_schema, endpoint.cache.table, /*strict=*/true);
                if (keys.empty()) {
                    continue;
                }
                const auto pinning = [&]() -> std::int64_t {
                    auto counted = db_adapter_->executeDuckLakeQueryWithResult(
                        "SELECT count(*) AS n FROM ducklake_snapshots('" + escapeSqlLiteral(catalog) + "') "
                        "WHERE snapshot_id >= " + std::to_string(*first_snapshot) +
                        " AND snapshot_id < (SELECT max(snapshot_id) FROM ducklake_snapshots('" +
                        escapeSqlLiteral(catalog) + "')) AND " + tableSnapshotPredicate(keys));
                    auto rows = crow::json::load(counted.data.dump());
                    if (rows && rows.t() == crow::json::type::List && rows.size() == 1 && rows[0].has("n") &&
                        rows[0]["n"].t() == crow::json::type::Number) {
                        return static_cast<std::int64_t>(rows[0]["n"].d());
                    }
                    return 0;
                };
                if (pinning() == 0) {
                    continue;
                }
                const auto retention = effectiveRetention(config_manager, endpoint.cache);
                const bool has_policy = retention.keep_last_snapshots || retention.max_snapshot_age;
                if (has_policy) {
                    result.expired_snapshots +=
                        applyRetention(catalog, ep_schema, endpoint.cache.table, retention).expired_snapshots;
                }
                const auto still = pinning();
                if (still == 0) {
                    continue;
                }
                std::ostringstream holder;
                holder << (endpoint.urlPath.empty() ? endpoint.getName() : endpoint.urlPath) << " (" << ep_schema
                       << "." << endpoint.cache.table << ": " << still << " snapshot(s) kept by ";
                if (retention.keep_last_snapshots) {
                    holder << "retention keep-last-snapshots " << *retention.keep_last_snapshots;
                } else if (retention.max_snapshot_age) {
                    holder << "retention max-snapshot-age " << *retention.max_snapshot_age;
                } else {
                    holder << "no retention policy; set cache.retention or ducklake.retention";
                }
                holder << ")";
                result.pinned_by.push_back(holder.str());
            }
            const auto bytes_after = referencedFileBytes(catalog);
            if (bytes_before && bytes_after && *bytes_before >= *bytes_after) {
                result.bytes_freed = *bytes_before - *bytes_after;
            }
            result.files_deleted = cleanupOldFiles(catalog);
        } catch (const std::exception& ex) {
            result.warning = std::string("snapshot expiry failed: ") + ex.what();
            CROW_LOG_WARNING << "Could not expire the snapshots of dropped table " << schema << "." << table
                             << ": " << ex.what();
        }
    } else {
        result.warning = "the dropped table's snapshots could not be identified; nothing was expired";
    }
    std::ostringstream message;
    message << "Dropped " << schema << "." << table << "; expired " << result.expired_snapshots
            << " snapshot(s), deleted " << result.files_deleted << " file(s), freed " << result.bytes_freed
            << " bytes";
    if (!result.pinned_by.empty()) {
        // Which endpoint's retention holds what is left, not a generic "later".
        message << "; files the dropped table shares with snapshots still retained by ";
        for (std::size_t i = 0; i < result.pinned_by.size(); ++i) {
            message << (i > 0 ? ", " : "") << result.pinned_by[i];
        }
        message << " are deleted when that retention expires those snapshots";
    }
    result.message = message.str();
    return result;
}

void CacheManager::initializeAuditTables(std::shared_ptr<ConfigManager> config_manager) {
    const auto& ducklakeConfig = config_manager->getDuckLakeConfig();
    if (!ducklakeConfig.enabled) {
        return;
    }

    const std::string catalog = ducklakeConfig.alias;
    const std::string auditSchema = "audit";
    
    // Create audit schema if it doesn't exist
    std::string createSchemaSQL = "CREATE SCHEMA IF NOT EXISTS " + catalog + "." + auditSchema;
    
    // Create sync_events audit table (DuckLake doesn't support PRIMARY KEY constraints)
    std::string createAuditTableSQL = R"(
        CREATE TABLE IF NOT EXISTS )" + catalog + "." + auditSchema + R"(.sync_events (
            event_id VARCHAR,
            endpoint_path VARCHAR NOT NULL,
            cache_table VARCHAR NOT NULL,
            cache_schema VARCHAR NOT NULL,
            sync_type VARCHAR NOT NULL,  -- 'full', 'append', 'merge', 'garbage_collection'
            status VARCHAR NOT NULL,     -- 'success', 'error', 'warning'
            message TEXT,
            snapshot_id VARCHAR,
            rows_affected BIGINT,
            sync_started_at TIMESTAMP,
            sync_completed_at TIMESTAMP,
            duration_ms BIGINT,
            "full" BOOLEAN       -- the refresh loaded the whole source (a keyword: quoted)
        )
    )";
    // Catalogs created before the column existed.
    const std::string addFullColumnSQL = "ALTER TABLE " + catalog + "." + auditSchema +
                                         ".sync_events ADD COLUMN IF NOT EXISTS \"full\" BOOLEAN";

    try {
        std::map<std::string, std::string> params;
        db_adapter_->executeDuckLakeQuery(createSchemaSQL, params);
        db_adapter_->executeDuckLakeQuery(createAuditTableSQL, params);
        try {
            db_adapter_->executeDuckLakeQuery(addFullColumnSQL, params);
        } catch (const std::exception& ex) {
            CROW_LOG_WARNING << "Could not add the `full` column to the cache audit table: " << ex.what();
        }
        CROW_LOG_INFO << "Initialized DuckLake audit tables in " << catalog << "." << auditSchema;
        // Bound whatever a previous run accumulated: the FIRST event this process
        // records prunes. Not here - it would delay warm-up on every start.
        audit_events_since_prune_.store(kAuditPruneEvery - 1);
    } catch (const std::exception& ex) {
        CROW_LOG_ERROR << "Failed to initialize audit tables: " << ex.what();
    }
}

void CacheManager::ensureCacheSchemaExists(const std::string& catalog, const std::string& schema) {
    // Skip if schema is "main" (default schema always exists)
    if (schema == "main") {
        return;
    }
    
    std::string createSchemaSQL = "CREATE SCHEMA IF NOT EXISTS " + catalog + "." + schema;
    
    try {
        std::map<std::string, std::string> params;
        db_adapter_->executeDuckLakeQuery(createSchemaSQL, params);
        CROW_LOG_DEBUG << "Ensured cache schema exists: " << catalog << "." << schema;
    } catch (const std::exception& ex) {
        CROW_LOG_WARNING << "Failed to create cache schema " << catalog << "." << schema << ": " << ex.what();
        // Don't throw - schema might already exist or be created by another process
    }
}

std::string CacheManager::formatUtcTimestamp(std::chrono::system_clock::time_point tp) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count();
    const std::time_t secs = static_cast<std::time_t>(ms / 1000);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &secs);
#else
    gmtime_r(&secs, &utc);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                  utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                  utc.tm_hour, utc.tm_min, utc.tm_sec, static_cast<int>(ms % 1000));
    return std::string(buf);
}

std::string CacheManager::buildSyncEventInsert(const std::string& catalog, const std::string& event_id,
                                               const EndpointConfig& endpoint, const std::string& sync_type,
                                               const std::string& status, const std::string& message,
                                               const SyncEventDetails& details) {
    const auto& cacheConfig = endpoint.cache;
    const auto quote = [](const std::string& v) { return "'" + escapeSqlLiteral(v) + "'"; };
    const auto ts = [&](const std::optional<std::chrono::system_clock::time_point>& tp) {
        // A missing instant falls back to "now" so the row still sorts where
        // it happened, exactly as every event was recorded before timing existed.
        return tp ? "TIMESTAMP " + quote(formatUtcTimestamp(*tp)) : std::string("CAST(CURRENT_TIMESTAMP AS TIMESTAMP)");
    };

    std::string duration = "NULL";
    if (details.started_at && details.completed_at) {
        const auto d = std::chrono::duration_cast<std::chrono::milliseconds>(
            *details.completed_at - *details.started_at).count();
        duration = std::to_string(d < 0 ? 0 : d);
    }
    const std::string rows = details.rows_affected ? std::to_string(*details.rows_affected) : "NULL";
    const std::string snapshot = details.snapshot_id ? quote(*details.snapshot_id) : "NULL";

    // A catalog-wide job (compaction, a dropped table's bookkeeping) has no
    // table: its schema stays empty too, rather than defaulting to "main".
    const std::string schema = cacheConfig.table.empty() ? cacheConfig.schema
                             : cacheConfig.schema.empty() ? std::string("main")
                                                          : cacheConfig.schema;
    return "INSERT INTO " + catalog + ".audit.sync_events "
           "(event_id, endpoint_path, cache_table, cache_schema, sync_type, status, message, "
           "snapshot_id, rows_affected, sync_started_at, sync_completed_at, duration_ms, \"full\") VALUES (" +
           quote(event_id) + ", " +
           quote(endpoint.urlPath) + ", " +
           quote(cacheConfig.table) + ", " +
           quote(schema) + ", " +
           quote(sync_type) + ", " +
           quote(status) + ", " +
           quote(message) + ", " +
           snapshot + ", " + rows + ", " +
           ts(details.started_at) + ", " + ts(details.completed_at) + ", " + duration + ", " +
           (details.full.value_or(false) ? "true" : "false") + ")";
}

void CacheManager::updateRefreshStats(const CacheKey& key, const std::string& endpoint_path, const std::string& sync_type,
                                      const std::string& status, const SyncEventDetails& details) {
    // GC and compaction are not refreshes; counting them would dilute refresh
    // durations (and compaction has no cache table to file them under).
    if (sync_type == "garbage_collection" || sync_type == "compaction") {
        return;
    }
    std::lock_guard<std::mutex> lock(refresh_stats_mutex_);
    auto& stats = refresh_stats_[key];
    stats.endpoint_path = endpoint_path;
    stats.catalog = key.catalog;
    stats.schema = key.schema;
    stats.table = key.table;
    stats.last_sync_type = sync_type;
    stats.last_status = status;
    stats.last_rows_affected = details.rows_affected;
    stats.last_snapshot_id = details.snapshot_id;
    stats.last_completed_at = details.completed_at;
    stats.last_warning = details.warning;
    stats.last_full = details.full.value_or(false);
    stats.refreshes += 1;
    if (status != "success") {
        stats.failures += 1;
    } else {
        stats.last_success_at = details.completed_at;
    }
    stats.last_duration_ms.reset();
    if (details.started_at && details.completed_at) {
        const int64_t d = std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::milliseconds>(
            *details.completed_at - *details.started_at).count());
        stats.last_duration_ms = d;
        stats.total_duration_ms += d;
        stats.max_duration_ms = std::max(stats.max_duration_ms, d);
    }
}

std::vector<CacheManager::CacheRefreshStats> CacheManager::getRefreshStats() const {
    std::lock_guard<std::mutex> lock(refresh_stats_mutex_);
    std::vector<CacheRefreshStats> out;
    out.reserve(refresh_stats_.size());
    for (const auto& [key, stats] : refresh_stats_) {
        out.push_back(stats);
    }
    return out;
}

std::map<std::string, std::string> CacheManager::getReadinessStates() const {
    std::lock_guard<std::mutex> lock(readiness_mutex_);
    std::map<std::string, std::string> out;
    for (const auto& [key, r] : readiness_) {
        std::string state;
        switch (r.state) {
            case ReadinessState::Starting: state = "starting"; break;
            case ReadinessState::Ready: state = r.stale ? "stale" : "ready"; break;
            case ReadinessState::Failed: state = "failed"; break;
            case ReadinessState::Unbuilt: state = "unbuilt"; break;
        }
        out[key.catalog + "." + key.schema + "." + key.table] = state;
    }
    return out;
}

void CacheManager::recordSyncEvent(std::shared_ptr<ConfigManager> config_manager, const EndpointConfig& endpoint, const std::string& sync_type, const std::string& status, const std::string& message, const SyncEventDetails& details) {
    // In-memory stats first and unconditionally: they do not depend on the
    // audit table existing, or on the INSERT below succeeding.
    updateRefreshStats(cacheKeyForEndpoint(config_manager, endpoint), endpoint.urlPath, sync_type, status, details);

    const auto& ducklakeConfig = config_manager->getDuckLakeConfig();
    if (!ducklakeConfig.enabled) {
        return;
    }

    try {
        const std::string eventId = newEventId();
        const std::string insertQuery = buildSyncEventInsert(ducklakeConfig.alias, eventId, endpoint,
                                                             sync_type, status, message, details);
        std::map<std::string, std::string> params;
        db_adapter_->executeDuckLakeQuery(insertQuery, params);
    } catch (const std::exception& ex) {
        CROW_LOG_WARNING << "Failed to record sync event: " << ex.what();
        return;
    }

    // Every INSERT above is a DuckLake snapshot. With a 10 s schedule that is 8640
    // a day, forever, and every refresh's snapshot lookups scan them all - a 0-row
    // refresh measured 27 ms on a fresh catalog and 56 ms a few thousand events
    // later. Amortised: one prune per kAuditPruneEvery events.
    if (audit_events_since_prune_.fetch_add(1) + 1 >= kAuditPruneEvery) {
        audit_events_since_prune_.store(0);
        pruneAuditLog(config_manager);
    }
}

void CacheManager::pruneAuditLog(std::shared_ptr<ConfigManager> config_manager) {
    const auto& ducklake = config_manager->getDuckLakeConfig();
    if (!ducklake.enabled || ducklake.read_only) {
        return;
    }
    const std::string catalog = ducklake.alias;
    const std::string table = quoteIdentifier(catalog) + ".audit.sync_events";
    try {
        // Rows: the newest kAuditRowsKept per cache table (garbage collection and
        // compaction events included, under their own table/blank keys).
        db_adapter_->executeDuckLakeQuery(
            "DELETE FROM " + table + " WHERE event_id IN (SELECT event_id FROM ("
            "SELECT event_id, row_number() OVER (PARTITION BY cache_schema, cache_table "
            "ORDER BY sync_completed_at DESC NULLS LAST, event_id DESC) AS rn FROM " + table +
            ") WHERE rn > " + std::to_string(kAuditRowsKept) + ")", {});
    } catch (const std::exception& ex) {
        CROW_LOG_WARNING << "Failed to prune the cache audit log: " << ex.what();
        return;
    }

    if (expireAuditSnapshots(catalog)) {
        cleanupOldFiles(catalog);
    }
}

bool CacheManager::expireAuditSnapshots(const std::string& catalog) {
    // History of the audit table is worth nothing, and its snapshots are what grows
    // the catalog. Worse, each one PINS the cache files visible at its version: an
    // audit INSERT committed between two full refreshes kept the older refresh's
    // parquet alive after retention expired that refresh's own snapshot - measured,
    // five refreshes under keep-last-snapshots: 1 left all five files on disk.
    // Same per-table, exclusive-only expiry as a cache table, so a snapshot shared
    // with anything else is never touched.
    const std::string expire_sql = buildExpireSql(catalog, "audit", "sync_events", kAuditSnapshotsKept, "");
    if (expire_sql.empty()) {
        return false;
    }
    try {
        db_adapter_->executeDuckLakeQuery(expire_sql, {});
        return true;
    } catch (const std::exception& ex) {
        CROW_LOG_WARNING << "Failed to expire cache audit snapshots: " << ex.what();
        return false;
    }
}

std::string CacheManager::newEventId() {
    // A random (version 4) UUID. The id used to be `evt_<epoch seconds>_<rand()>`
    // with rand() never seeded, so every process produced the same sequence and two
    // events in the same second of two runs collided.
    static thread_local std::mt19937_64 rng{[] {
        std::random_device rd;
        std::seed_seq seq{rd(), rd(), rd(), rd(),
                          static_cast<unsigned>(std::chrono::high_resolution_clock::now().time_since_epoch().count())};
        return std::mt19937_64(seq);
    }()};
    std::uniform_int_distribution<uint64_t> dist;
    uint64_t hi = dist(rng);
    uint64_t lo = dist(rng);
    hi = (hi & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;   // version 4
    lo = (lo & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;   // RFC 4122 variant
    char buf[37];
    std::snprintf(buf, sizeof(buf), "%08x-%04x-%04x-%04x-%012llx",
                  static_cast<unsigned>(hi >> 32), static_cast<unsigned>((hi >> 16) & 0xFFFF),
                  static_cast<unsigned>(hi & 0xFFFF), static_cast<unsigned>(lo >> 48),
                  static_cast<unsigned long long>(lo & 0xFFFFFFFFFFFFULL));
    return std::string(buf);
}

CacheManager::ActiveRefreshGuard::ActiveRefreshGuard(CacheManager& manager)
    : manager_(manager), tid_(std::this_thread::get_id()) {
    std::lock_guard<std::mutex> lock(manager_.active_refresh_mutex_);
    manager_.active_refresh_threads_.insert(tid_);
}

CacheManager::ActiveRefreshGuard::~ActiveRefreshGuard() {
    std::lock_guard<std::mutex> lock(manager_.active_refresh_mutex_);
    manager_.active_refresh_threads_.erase(tid_);
}

void CacheManager::interruptRefreshesForShutdown(std::chrono::milliseconds budget) {
    // SIGTERM used to wait for whatever populate query was running - a warm-up
    // measured ~80 s - because the warm-up thread and the scheduler are joined
    // before the catalog is detached. They are interrupted instead (the same
    // duckdb_interrupt mechanism MCP cancellation uses, #111): the refresh fails,
    // its transaction rolls back, and the previous snapshot is what the next start
    // serves.
    shutdown_requested_.store(true);
    const auto deadline = std::chrono::steady_clock::now() + budget;
    bool logged = false;
    while (true) {
        std::vector<std::thread::id> threads;
        {
            std::lock_guard<std::mutex> lock(active_refresh_mutex_);
            threads.assign(active_refresh_threads_.begin(), active_refresh_threads_.end());
        }
        if (threads.empty()) {
            return;
        }
        if (!logged) {
            CROW_LOG_INFO << "Shutdown: interrupting " << threads.size() << " running cache refresh(es)";
            logged = true;
        }
        // Repeated, not once: a script runs statement by statement, and an interrupt
        // landing between two of them has nothing to cancel.
        for (const auto& tid : threads) {
            interruptActiveExecutor(tid);
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            CROW_LOG_WARNING << "Shutdown: a cache refresh is still running after the interrupt budget";
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

bool CacheManager::shutdownRequested() const {
    return shutdown_requested_.load();
}

std::optional<std::chrono::system_clock::time_point> CacheManager::lastRefreshCompletedAt(
        std::shared_ptr<ConfigManager> config_manager, const EndpointConfig& endpoint) const {
    const CacheKey key = cacheKeyForEndpoint(config_manager, endpoint);
    std::lock_guard<std::mutex> lock(refresh_stats_mutex_);
    const auto it = refresh_stats_.find(key);
    if (it == refresh_stats_.end()) {
        return std::nullopt;
    }
    return it->second.last_completed_at;
}

std::optional<std::chrono::system_clock::time_point> CacheManager::newestSnapshotTime(
        std::shared_ptr<ConfigManager> config_manager, const EndpointConfig& endpoint) {
    return newestSnapshotTimeForKey(cacheKeyForEndpoint(config_manager, endpoint));
}

std::optional<std::chrono::system_clock::time_point> CacheManager::dataTimestamp(
        const std::string& catalog, const std::string& schema, const std::string& table) {
    const CacheKey key{catalog, schema, table};
    {
        std::lock_guard<std::mutex> lock(refresh_stats_mutex_);
        const auto it = refresh_stats_.find(key);
        if (it != refresh_stats_.end() && it->second.last_success_at) {
            return it->second.last_success_at;
        }
        const auto memo = snapshot_time_memo_.find(key);
        if (memo != snapshot_time_memo_.end()) {
            return memo->second;
        }
    }
    // Outside the lock: a catalog query. Once per table per process, so the
    // metrics endpoint stays cheap.
    const auto found = newestSnapshotTimeForKey(key);
    std::lock_guard<std::mutex> lock(refresh_stats_mutex_);
    snapshot_time_memo_[key] = found;
    return found;
}

std::optional<std::chrono::system_clock::time_point> CacheManager::newestSnapshotTimeForKey(const CacheKey& key) {
    try {
        const std::string query =
            "SELECT CAST(epoch_ms(snapshot_time) AS BIGINT) AS ms FROM ducklake_snapshots('" +
            escapeSqlLiteral(key.catalog) + "') WHERE " +
            tableSnapshotPredicate(tableChangeKeys(key.catalog, key.schema, key.table, /*strict=*/false)) +
            " ORDER BY snapshot_id DESC LIMIT 1";
        auto result = db_adapter_->executeDuckLakeQueryWithResult(query);
        auto rows = crow::json::load(result.data.dump());
        if (rows && rows.t() == crow::json::type::List && rows.size() > 0 && rows[0].has("ms") &&
            rows[0]["ms"].t() == crow::json::type::Number) {
            return std::chrono::system_clock::time_point(
                std::chrono::milliseconds(static_cast<int64_t>(rows[0]["ms"].d())));
        }
    } catch (const std::exception& ex) {
        CROW_LOG_DEBUG << "No snapshot time for " << key.schema << "." << key.table << ": " << ex.what();
    }
    return std::nullopt;
}

namespace {
// The audit row a catalog-wide maintenance task is filed under: no endpoint, no table.
EndpointConfig catalogMaintenanceEndpoint() {
    EndpointConfig endpoint;
    endpoint.urlPath = "";
    endpoint.cache.table = "";
    endpoint.cache.schema = "";
    return endpoint;
}
} // namespace

CacheManager::CompactionResult CacheManager::performCompaction(std::shared_ptr<ConfigManager> config_manager,
                                                               const std::string& schema,
                                                               const std::string& table) {
    CompactionResult out;
    const auto& ducklake = config_manager->getDuckLakeConfig();
    if (!ducklake.enabled || ducklake.read_only) {
        out.status = "skipped";
        out.message = !ducklake.enabled ? "DuckLake is not enabled" : "the catalog is attached read-only";
        return out;
    }
    const std::string catalog = escapeSqlLiteral(ducklake.alias);
    const bool one_table = !table.empty();
    const std::string table_schema = schema.empty() ? "main" : schema;
    auto catalog_lock = lockCatalogWrites();
    CROW_LOG_INFO << "Running DuckLake compaction for "
                  << (one_table ? "table " + table_schema + "." + table : "catalog: " + ducklake.alias);

    SyncEventDetails details;
    details.started_at = std::chrono::system_clock::now();
    std::vector<std::string> failures;
    // Once per CATALOG. It used to loop over cached endpoints and run the same
    // catalog-wide call once for each of them.
    //
    //  - merge_adjacent_files: the small files every incremental refresh adds;
    //  - rewrite_data_files: files whose rows are mostly deleted, which is what a
    //    merge-mode cache accumulates (each UPDATE is a delete plus an insert);
    //  - cleanup_old_files: the files the two above replaced. Without it compaction
    //    only ADDED files to disk.
    // delete_threshold: DuckLake rewrites a file only above 95% deleted rows by
    // default, and a merge cache's delete files did not get there - measured,
    // compaction left 52 files and 24 delete files exactly as it found them while
    // each refresh got ~30 ms slower.
    std::string target_args = "'" + catalog + "'";
    if (one_table) {
        target_args += ", '" + escapeSqlLiteral(table) + "', schema => '" + escapeSqlLiteral(table_schema) + "'";
    }
    std::string rewrite_args = target_args;
    if (ducklake.compaction.delete_threshold) {
        std::ostringstream threshold;
        threshold.imbue(std::locale::classic());
        threshold << *ducklake.compaction.delete_threshold;
        rewrite_args += ", delete_threshold => " + threshold.str();
    }
    std::optional<std::int64_t> files_rewritten;
    // DuckLake answers merge/rewrite with one row per table it touched; the
    // count of files it replaced is the column whose name says so, when the
    // installed version reports one.
    const auto count_rewritten = [&files_rewritten](const QueryResult& result) {
        auto rows = crow::json::load(result.data.dump());
        if (!rows || rows.t() != crow::json::type::List) {
            return;
        }
        for (const auto& row : rows) {
            if (row.t() != crow::json::type::Object) {
                continue;
            }
            for (const auto& column : row.keys()) {
                if ((column == "files_processed" || column == "files_rewritten" || column == "files_merged") &&
                    row[column].t() == crow::json::type::Number) {
                    files_rewritten = files_rewritten.value_or(0) + static_cast<std::int64_t>(row[column].d());
                }
            }
        }
    };
    for (const std::string& sql : {std::string("CALL ducklake_merge_adjacent_files(") + target_args + ")",
                                   std::string("CALL ducklake_rewrite_data_files(") + rewrite_args + ")"}) {
        try {
            count_rewritten(db_adapter_->executeDuckLakeQueryWithResult(sql));
        } catch (const std::exception& ex) {
            CROW_LOG_WARNING << "DuckLake compaction step failed (" << sql << "): " << ex.what();
            failures.push_back(ex.what());
        }
    }
    try {
        db_adapter_->executeDuckLakeQuery("CALL ducklake_cleanup_old_files('" + catalog + "', cleanup_all => true)", {});
    } catch (const std::exception& ex) {
        CROW_LOG_WARNING << "DuckLake compaction step failed (cleanup_old_files): " << ex.what();
        failures.push_back(ex.what());
    }
    details.completed_at = std::chrono::system_clock::now();
    out.duration_ms = std::max<std::int64_t>(
        0, std::chrono::duration_cast<std::chrono::milliseconds>(*details.completed_at - *details.started_at).count());
    out.files_rewritten = files_rewritten;
    const std::string subject = one_table ? table_schema + "." + table : "catalog " + ducklake.alias;
    out.status = failures.empty() ? "compacted" : "error";
    out.message = failures.empty() ? "Compacted " + subject : "Compaction of " + subject + " failed: " + failures.front();
    // Filed under the table it compacted; a catalog-wide run under no table AND
    // no schema (it used to be recorded as schema "main").
    EndpointConfig filed_under = catalogMaintenanceEndpoint();
    if (one_table) {
        filed_under.cache.schema = table_schema;
        filed_under.cache.table = table;
    }
    recordSyncEvent(config_manager, filed_under, "compaction", failures.empty() ? "success" : "error",
                    out.message, details);
    CROW_LOG_INFO << "DuckLake compaction of " << subject << " finished"
                  << (failures.empty() ? "" : " with errors");
    return out;
}

std::optional<std::chrono::system_clock::time_point> CacheManager::lastCompactionTime(
        std::shared_ptr<ConfigManager> config_manager) {
    // From the audit log, so the cadence survives restarts and the one-shot
    // `flapi cache refresh` (a fresh process every time) can tell whether
    // compaction is due.
    const auto& ducklake = config_manager->getDuckLakeConfig();
    if (!ducklake.enabled) {
        return std::nullopt;
    }
    try {
        auto result = db_adapter_->executeDuckLakeQueryWithResult(
            "SELECT CAST(epoch_ms(max(sync_completed_at)) AS BIGINT) AS ms FROM " +
            quoteIdentifier(ducklake.alias) + ".audit.sync_events WHERE sync_type = 'compaction' AND cache_table = ''");
        auto rows = crow::json::load(result.data.dump());
        if (rows && rows.t() == crow::json::type::List && rows.size() > 0 && rows[0].has("ms") &&
            rows[0]["ms"].t() == crow::json::type::Number) {
            return std::chrono::system_clock::time_point(
                std::chrono::milliseconds(static_cast<int64_t>(rows[0]["ms"].d())));
        }
    } catch (const std::exception& ex) {
        CROW_LOG_DEBUG << "No previous compaction recorded: " << ex.what();
    }
    return std::nullopt;
}

bool CacheManager::compactionDue(std::shared_ptr<ConfigManager> config_manager,
                                 std::chrono::system_clock::time_point now) {
    const auto& compaction = config_manager->getDuckLakeConfig().compaction;
    if (!compaction.enabled || !compaction.schedule) {
        return false;
    }
    const auto interval = TimeInterval::parseSchedule(*compaction.schedule);
    if (!interval) {
        return false;
    }
    const auto last = lastCompactionTime(config_manager);
    return !last || now - *last >= *interval;
}

std::optional<std::chrono::seconds> TimeInterval::parseInterval(const std::string& interval) {
    if (interval.empty()) {
        return std::nullopt;
    }

    try {
        static const std::regex pattern("^(\\d{1,9})([smhd])$");
        std::smatch matches;

        if (!std::regex_match(interval, matches, pattern)) {
            return std::nullopt;
        }

        // 64-bit arithmetic (a `30000d` overflowed int seconds) and strictly positive: a
        // zero interval made the scheduler refresh on every scan.
        const std::int64_t value = std::stoll(matches[1].str());
        std::int64_t seconds = 0;
        switch (matches[2].str()[0]) {
            case 's': seconds = value; break;
            case 'm': seconds = value * 60; break;
            case 'h': seconds = value * 3600; break;
            case 'd': seconds = value * 86400; break;
            default: return std::nullopt;
        }
        // At most ~10 years: beyond that it is a typo, and keeps every consumer far from overflow.
        constexpr std::int64_t kMaxSeconds = 10LL * 365 * 86400;
        if (seconds <= 0 || seconds > kMaxSeconds) {
            return std::nullopt;
        }
        return std::chrono::seconds(seconds);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<std::chrono::seconds> TimeInterval::parseSchedule(const std::string& schedule) {
    // The cron-style shorthands the examples and CONFIG_REFERENCE already used for
    // compaction. Full cron expressions are not supported: a maintenance task needs a
    // cadence, not a wall-clock alignment.
    if (schedule == "@hourly") {
        return std::chrono::seconds(3600);
    }
    if (schedule == "@daily" || schedule == "@midnight") {
        return std::chrono::seconds(86400);
    }
    if (schedule == "@weekly") {
        return std::chrono::seconds(7 * 86400);
    }
    return parseInterval(schedule);
}

} // namespace flapi
