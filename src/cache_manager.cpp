#include <sstream>
#include <fstream>
#include <stdexcept>
#include <crow.h>
#include <regex>
#include <ctime>
#include <cstdlib>
#include <exception>
#include <tuple>

#include "cache_manager.hpp"
#include "database_manager.hpp"
#include "database_manager_cache_adapter.hpp"

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
    
    // Initialize audit tables first
    initializeAuditTables(config_manager);
    
    const auto& cache_schema = config_manager->getCacheSchema();

    std::map<std::string, std::string> params;
    for (auto &endpoint : *config_manager->getEndpoints()) 
    {
        // Warmup: refresh caches only for endpoints with cache enabled and a table defined
        if (endpoint.cache.enabled && !endpoint.cache.table.empty()) {
            markCacheStarting(config_manager, endpoint);
            try {
                if (refreshCache(config_manager, endpoint, params)) {
                    markCacheReady(config_manager, endpoint);
                } else {
                    while (getEndpointReadiness(config_manager, endpoint).state == ReadinessState::Starting) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                }
            } catch (const std::exception& ex) {
                CROW_LOG_ERROR << "Cache warmup failed for " << endpoint.cache.table << ": " << ex.what();
                markCacheFailed(config_manager, endpoint, ex.what());
            } catch (...) {
                CROW_LOG_ERROR << "Cache warmup failed for " << endpoint.cache.table << ": unknown error";
                markCacheFailed(config_manager, endpoint, "unknown error");
            }
        }
    }
    CROW_LOG_INFO << "Finished warming up endpoint caches! Let's go!";
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

bool CacheManager::refreshCache(std::shared_ptr<ConfigManager> config_manager, const EndpointConfig& endpoint, std::map<std::string, std::string>& params) {
    const CacheKey key = cacheKeyForEndpoint(config_manager, endpoint);
    if (!enterRefresh(key)) {
        CROW_LOG_INFO << "Skipping duplicate in-flight cache refresh for " << key.schema << "." << key.table;
        return false;
    }

    try {
        refreshDuckLakeCache(config_manager, endpoint, params);
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
    readiness_[key] = CacheReadiness{ReadinessState::Failed, key.catalog, key.schema, key.table, error};
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

std::optional<CacheManager::CacheReadiness> CacheManager::readinessBlock(std::shared_ptr<ConfigManager> config_manager, const EndpointConfig& endpoint) const {
    if (!endpoint.cache.enabled || endpoint.cache.table.empty()) {
        return std::nullopt;
    }

    auto readiness = getEndpointReadiness(config_manager, endpoint);
    if (readiness.state == ReadinessState::Ready) {
        return std::nullopt;
    }
    return readiness;
}

crow::json::wvalue CacheManager::readinessBlockJson(const CacheReadiness& readiness) {
    crow::json::wvalue errorResponse;
    errorResponse["error"] = "cache_warming";
    errorResponse["table"] = readiness.table;
    if (readiness.state == ReadinessState::Failed) {
        errorResponse["message"] = "Cache for this endpoint failed to build";
        errorResponse["detail"] = readiness.error;
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
        } else if (readiness.state == ReadinessState::Failed) {
            ++summary.failed;
            summary.failed_caches.push_back(readiness);
        } else {
            summary.pending_caches.push_back(readiness);
        }
    }
    return summary;
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

void CacheManager::refreshDuckLakeCache(std::shared_ptr<ConfigManager> config_manager, const EndpointConfig& endpoint, std::map<std::string, std::string> params) {
    const auto& cacheConfig = endpoint.cache;
    const auto& ducklakeConfig = config_manager->getDuckLakeConfig();
    const std::string catalog = ducklakeConfig.alias;
    const std::string schema = cacheConfig.schema.empty() ? "main" : cacheConfig.schema;
    const std::string table = cacheConfig.table;

    // Ensure cache schema exists before processing cache template
    ensureCacheSchemaExists(catalog, schema);

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
    if (cacheConfig.hasCursor()) {
        const std::string watermark =
            fetchCursorWatermark(catalog, schema, table, cacheConfig.cursor->column);
        if (!watermark.empty()) {
            params["previousSnapshotTimestamp"] = watermark;
        }
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

    const std::string rendered = db_adapter_->renderCacheTemplate(endpoint, cacheConfig, params);

    try {
        db_adapter_->executeDuckLakeQuery(rendered, params);
        recordSyncEvent(config_manager, endpoint, determineCacheMode(cacheConfig), "success", "Cache refreshed successfully");
    } catch (const std::exception& ex) {
        recordSyncEvent(config_manager, endpoint, determineCacheMode(cacheConfig), "error", ex.what());
        throw;
    }

    // trigger retention expiry if requested
    if (cacheConfig.retention.keep_last_snapshots || cacheConfig.retention.max_snapshot_age) {
        std::string expireSql;
        if (cacheConfig.retention.max_snapshot_age) {
            // Convert time interval to proper timestamp format
            std::string timeInterval = cacheConfig.retention.max_snapshot_age.value();
            std::string timestampExpr = "CAST(CURRENT_TIMESTAMP AS TIMESTAMP) - INTERVAL '" + timeInterval + "'";
            expireSql = "CALL ducklake_expire_snapshots('" + catalog + "', older_than => " + timestampExpr + ")";
        } else {
            // Count-based expiry: keep the newest N snapshots, expire the rest.
            //
            // This used to emit `versions => ARRAY[0:N]`, which is not DuckDB
            // syntax at all - `SELECT ARRAY[0:10]` is a parser error - so the
            // CALL failed every time. The failure was caught and logged at
            // WARNING, so count-based retention has never run: an operator who
            // set `keep-last-snapshots` believed old snapshots were being
            // pruned while they accumulated without bound.
            //
            // `versions` takes an explicit list of snapshot ids, so the newest
            // N have to be resolved first. Nothing to expire is the common
            // case early in a cache's life and is not an error.
            expireSql = buildCountBasedExpireSql(
                catalog, schema, table, cacheConfig.retention.keep_last_snapshots.value());
        }
        if (expireSql.empty()) {
            return;   // nothing old enough to expire yet
        }
        try {
            db_adapter_->executeDuckLakeQuery(expireSql, params);
        } catch (const std::exception& ex) {
            CROW_LOG_WARNING << "Failed to expire DuckLake snapshots for " << schema << "." << table << ": " << ex.what();
        }
    }
}

std::string CacheManager::fetchCursorWatermark(const std::string& catalog,
                                               const std::string& schema,
                                               const std::string& table,
                                               const std::string& cursor_column) {
    if (cursor_column.empty()) {
        return {};
    }
    try {
        const std::string query =
            "SELECT CAST(max(" + cursor_column + ") AS VARCHAR) AS watermark FROM " +
            catalog + "." + schema + "." + table;
        auto result = db_adapter_->executeDuckLakeQueryWithResult(query);
        auto rows = crow::json::load(result.data.dump());
        if (rows && rows.t() == crow::json::type::List && rows.size() > 0 &&
            rows[0].has("watermark") &&
            rows[0]["watermark"].t() == crow::json::type::String) {
            return rows[0]["watermark"].s();
        }
    } catch (const std::exception& ex) {
        // Table not created yet, or the cursor column is not in it. The
        // snapshot timestamp remains the fallback.
        CROW_LOG_DEBUG << "No cursor watermark for " << schema << "." << table
                       << " (" << cursor_column << "): " << ex.what();
    }
    return {};
}

std::vector<std::string> CacheManager::tableChangeKeys(const std::string& catalog,
                                                       const std::string& schema,
                                                       const std::string& table) {
    // `changes` is a MAP(VARCHAR, VARCHAR[]) that names tables by id for row
    // changes and by `schema.table` for creates, so both forms count.
    std::vector<std::string> keys;
    keys.push_back(schema + "." + table);
    try {
        const std::string query =
            "SELECT CAST(table_id AS VARCHAR) AS table_id FROM ducklake_table_info('" +
            catalog + "') WHERE table_name = '" + table + "' LIMIT 1";
        auto result = db_adapter_->executeDuckLakeQueryWithResult(query);
        auto rows = crow::json::load(result.data.dump());
        if (rows && rows.t() == crow::json::type::List && rows.size() > 0 &&
            rows[0].has("table_id") && rows[0]["table_id"].t() == crow::json::type::String) {
            keys.push_back(rows[0]["table_id"].s());
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
        out << "list_contains(flatten(map_values(changes)), '" << keys[i] << "')";
    }
    out << ")";
    return out.str();
}

std::string CacheManager::buildCountBasedExpireSql(const std::string& catalog,
                                                   const std::string& schema,
                                                   const std::string& table,
                                                   std::size_t keep_last) {
    if (keep_last == 0) {
        return {};   // keeping nothing is not a retention policy; refuse it
    }

    std::vector<std::int64_t> expire_ids;
    try {
        // Every cached endpoint shares ONE DuckLake catalog, so a
        // catalog-wide snapshot list is every endpoint's history. This query
        // used to be exactly that, while `keep-last-snapshots` is configured
        // per endpoint: an endpoint with keep_last=2 refreshing for the third
        // time called ducklake_expire_snapshots on every other endpoint's
        // snapshots too, destroying their time-travel history and their
        // incremental watermark. The existing test uses a single table, so it
        // could not see it.
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
        const auto keys = tableChangeKeys(catalog, schema, table);

        // "Touched nothing but this table": every entry in the snapshot's
        // change list is one of this table's keys. Built from resolved
        // literals - a subquery inside the lambda is a binder error.
        std::ostringstream others;
        others << "len(list_filter(flatten(map_values(changes)), x -> ";
        for (std::size_t i = 0; i < keys.size(); ++i) {
            if (i > 0) {
                others << " AND ";
            }
            others << "x <> '" << keys[i] << "'";
        }
        others << ")) = 0";

        const std::string query =
            "SELECT snapshot_id FROM ducklake_snapshots('" + catalog + "') "
            "WHERE " + tableSnapshotPredicate(keys) + " AND " + others.str() + " "
            "ORDER BY snapshot_id DESC";
        auto result = db_adapter_->executeDuckLakeQueryWithResult(query);
        auto rows = crow::json::load(result.data.dump());
        if (!(rows && rows.t() == crow::json::type::List)) {
            return {};
        }
        for (std::size_t i = keep_last; i < rows.size(); ++i) {
            const auto& row = rows[i];
            if (row.has("snapshot_id") && row["snapshot_id"].t() == crow::json::type::Number) {
                expire_ids.push_back(static_cast<std::int64_t>(row["snapshot_id"].d()));
            }
        }
    } catch (const std::exception& ex) {
        CROW_LOG_WARNING << "Could not list DuckLake snapshots for count-based retention: " << ex.what();
        return {};
    }

    if (expire_ids.empty()) {
        return {};
    }

    std::ostringstream sql;
    sql << "CALL ducklake_expire_snapshots('" << catalog << "', versions => [";
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
            "SELECT snapshot_id, snapshot_time FROM ducklake_snapshots('" + catalog + "') "
            "WHERE " + tableSnapshotPredicate(tableChangeKeys(catalog, schema, table)) + " "
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
    params.emplace("cacheCatalog", config_manager->getDuckLakeConfig().alias);
    params.emplace("cacheSchema", schema);
    params.emplace("cacheTable", cacheConfig.table);
}

void CacheManager::performGarbageCollection(std::shared_ptr<ConfigManager> config_manager, const EndpointConfig& endpoint, const std::vector<std::string> previousTableNames) {
    const auto& cacheConfig = endpoint.cache;
    if (!cacheConfig.retention.keep_last_snapshots && !cacheConfig.retention.max_snapshot_age) {
        return;
    }

    std::map<std::string, std::string> params;
    params["catalog"] = config_manager->getDuckLakeConfig().alias;
    params["schema"] = cacheConfig.schema.empty() ? "main" : cacheConfig.schema;
    params["table"] = cacheConfig.table;

    // Use a simple time-based expiry for garbage collection
    std::string expireSql = "CALL ducklake_expire_snapshots('" + params["catalog"] + "', older_than => CAST(CURRENT_TIMESTAMP AS TIMESTAMP) - INTERVAL '1 day')";
    try {
        db_adapter_->executeDuckLakeQuery(expireSql, params);
        recordSyncEvent(config_manager, endpoint, "garbage_collection", "success", "Expired old snapshots");
    } catch (const std::exception& ex) {
        CROW_LOG_WARNING << "Failed to expire snapshots for " << params["schema"] << "." << params["table"] << ": " << ex.what();
        recordSyncEvent(config_manager, endpoint, "garbage_collection", "error", ex.what());
    }
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
            duration_ms BIGINT
        )
    )";

    try {
        std::map<std::string, std::string> params;
        db_adapter_->executeDuckLakeQuery(createSchemaSQL, params);
        db_adapter_->executeDuckLakeQuery(createAuditTableSQL, params);
        CROW_LOG_INFO << "Initialized DuckLake audit tables in " << catalog << "." << auditSchema;
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

void CacheManager::recordSyncEvent(std::shared_ptr<ConfigManager> config_manager, const EndpointConfig& endpoint, const std::string& sync_type, const std::string& status, const std::string& message) {
    const auto& ducklakeConfig = config_manager->getDuckLakeConfig();
    if (!ducklakeConfig.enabled) {
        return;
    }

    const std::string catalog = ducklakeConfig.alias;
    const std::string auditSchema = "audit";
    const auto& cacheConfig = endpoint.cache;
    
    std::string insertSQL = R"(
        INSERT INTO )" + catalog + "." + auditSchema + R"(.sync_events 
        (endpoint_path, cache_table, cache_schema, sync_type, status, message)
        VALUES (?, ?, ?, ?, ?, ?)
    )";

    try {
        // For now, use a simple query approach - in production, prepared statements would be better
        std::string escapedMessage = message;
        std::replace(escapedMessage.begin(), escapedMessage.end(), '\'', '"');
        
        std::string eventId = "evt_" + std::to_string(std::time(nullptr)) + "_" + std::to_string(rand());
        std::string insertQuery = "INSERT INTO " + catalog + "." + auditSchema + ".sync_events " +
            "(event_id, endpoint_path, cache_table, cache_schema, sync_type, status, message, sync_started_at, sync_completed_at) VALUES (" +
            "'" + eventId + "', " +
            "'" + endpoint.urlPath + "', " +
            "'" + cacheConfig.table + "', " +
            "'" + (cacheConfig.schema.empty() ? "main" : cacheConfig.schema) + "', " +
            "'" + sync_type + "', " +
            "'" + status + "', " +
            "'" + escapedMessage + "', " +
            "CURRENT_TIMESTAMP, CURRENT_TIMESTAMP)";

        std::map<std::string, std::string> params;
        db_adapter_->executeDuckLakeQuery(insertQuery, params);

    } catch (const std::exception& ex) {
        CROW_LOG_WARNING << "Failed to record sync event: " << ex.what();
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
