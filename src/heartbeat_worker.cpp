#include "heartbeat_worker.hpp"

#include <algorithm>
#include <set>

namespace flapi {

namespace {

std::string cacheTableKey(const DuckLakeConfig& ducklake, const EndpointConfig& endpoint) {
    const std::string schema = endpoint.cache.schema.empty() ? "main" : endpoint.cache.schema;
    return ducklake.alias + "." + schema + "." + endpoint.cache.table;
}

bool isScheduledCache(const EndpointConfig& endpoint) {
    return endpoint.cache.enabled && !endpoint.cache.table.empty() && endpoint.cache.schedule;
}

} // namespace

HeartbeatWorker::HeartbeatWorker(std::shared_ptr<ConfigManager> config_manager, APIServer& api_server)
    : config_manager(config_manager), api_server(api_server), running(false),
      started_at(std::chrono::system_clock::now()) {}

HeartbeatWorker::~HeartbeatWorker() {
    stop();
}

void HeartbeatWorker::start() {
    if (!running.exchange(true)) {
        worker_thread = std::thread(&HeartbeatWorker::workerLoop, this);
    }
}

void HeartbeatWorker::stop() {
    if (running.exchange(false)) {
        cv.notify_one(); // Wake up the worker thread
        if (worker_thread.joinable()) {
            worker_thread.join();
        }
    }
}

std::chrono::seconds HeartbeatWorker::CacheScanInterval(const DuckLakeConfig& ducklake_config,
                                                        const GlobalHeartbeatConfig& heartbeat_config,
                                                        const std::vector<EndpointConfig>& endpoints) {
    constexpr std::chrono::seconds kFloor{1};
    if (ducklake_config.scheduler.scan_interval) {
        if (const auto parsed = TimeInterval::parseInterval(*ducklake_config.scheduler.scan_interval)) {
            return std::max(kFloor, *parsed);
        }
    }
    // `scan-interval` was parsed and read by nothing: schedules were checked once per
    // heartbeat.worker-interval, so a `schedule: 10s` cache refreshed once a minute.
    auto interval = heartbeat_config.workerInterval;
    for (const auto& endpoint : endpoints) {
        if (!isScheduledCache(endpoint)) {
            continue;
        }
        if (const auto schedule = TimeInterval::parseInterval(*endpoint.cache.schedule)) {
            interval = std::min(interval, *schedule);
        }
    }
    return std::max(kFloor, interval);
}

bool HeartbeatWorker::RefreshDue(std::optional<std::chrono::system_clock::time_point> last_refresh,
                                 std::chrono::seconds interval,
                                 std::chrono::system_clock::time_point now) {
    if (!last_refresh) {
        return true;
    }
    return now - *last_refresh >= interval;
}

void HeartbeatWorker::logSchedulerSetup(std::chrono::seconds scan_interval) {
    const auto& ducklake = config_manager->getDuckLakeConfig();
    const auto endpoints = config_manager->getEndpoints();
    std::size_t scheduled = 0;
    for (const auto& endpoint : *endpoints) {
        if (isScheduledCache(endpoint)) {
            ++scheduled;
        }
    }
    if (scheduled == 0 || !ducklake.enabled) {
        return;
    }
    if (ducklake.read_only) {
        CROW_LOG_INFO << "Cache scheduler off: the DuckLake catalog is read-only; " << scheduled
                      << " cache.schedule setting(s) are not run by this process";
        return;
    }
    if (!ducklake.scheduler.enabled) {
        CROW_LOG_WARNING << "Cache scheduler off (ducklake.scheduler.enabled: false): " << scheduled
                         << " endpoint(s) declare cache.schedule and will NOT be refreshed on schedule";
        return;
    }
    CROW_LOG_INFO << "Cache scheduler on: " << scheduled << " scheduled cache(s), checked every "
                  << scan_interval.count() << " s"
                  << (ducklake.scheduler.scan_interval ? " (ducklake.scheduler.scan-interval)" : "");
    for (const auto& endpoint : *endpoints) {
        if (!isScheduledCache(endpoint)) {
            continue;
        }
        const auto schedule = TimeInterval::parseInterval(*endpoint.cache.schedule);
        if (schedule && *schedule < scan_interval) {
            CROW_LOG_WARNING << "Endpoint " << endpoint.urlPath << ": cache.schedule " << *endpoint.cache.schedule
                             << " is shorter than the scheduler's scan interval (" << scan_interval.count()
                             << " s); it refreshes at most once per scan";
        }
    }
}

void HeartbeatWorker::workerLoop() {
    // Two cadences on one thread: endpoint heartbeats every worker-interval, cache
    // schedules every scan interval. The first cache scan is one scan interval in,
    // not immediate: warm-up is still starting at that point, and a scan there only
    // raced it.
    const auto heartbeat_interval = config_manager->getGlobalHeartbeatConfig().workerInterval;
    auto scan_interval = CacheScanInterval(config_manager->getDuckLakeConfig(),
                                           config_manager->getGlobalHeartbeatConfig(),
                                           *config_manager->getEndpoints());
    logSchedulerSetup(scan_interval);

    auto next_heartbeat = std::chrono::steady_clock::now();
    auto next_scan = std::chrono::steady_clock::now() + scan_interval;

    while (running) {
        const auto now = std::chrono::steady_clock::now();
        const bool heartbeat_enabled = config_manager->getGlobalHeartbeatConfig().enabled;
        if (heartbeat_enabled && now >= next_heartbeat) {
            // Held for the loop: the config service swaps the endpoint list
            // copy-on-write, and a range-for over `*getEndpoints()` keeps no owner.
            const auto endpoints = config_manager->getEndpoints();
            for (const auto& endpoint : *endpoints) {
                if (!running) break; // Check if we should exit
                if (endpoint.heartbeat.enabled) {
                    performHeartbeat(endpoint);
                }
            }
            next_heartbeat = std::chrono::steady_clock::now() + heartbeat_interval;
        }

        if (running && std::chrono::steady_clock::now() >= next_scan) {
            performDuckLakeScheduledTasks();
            // Recomputed: the config service can add or change a schedule at runtime.
            scan_interval = CacheScanInterval(config_manager->getDuckLakeConfig(),
                                              config_manager->getGlobalHeartbeatConfig(),
                                              *config_manager->getEndpoints());
            next_scan = std::chrono::steady_clock::now() + scan_interval;
        }

        auto wake = next_scan;
        if (heartbeat_enabled) {
            wake = std::min(wake, next_heartbeat);
        }
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait_until(lock, wake, [this] { return !running; });
    }
}

void HeartbeatWorker::performHeartbeat(const EndpointConfig& endpoint)
{
    CROW_LOG_DEBUG << "Performing heartbeat for endpoint " << endpoint.urlPath;

    // Direct into the handler, not through the HTTP router (#141). This is the
    // same shape performDuckLakeScheduledTasks() below already used for
    // refreshCache(): a background task reaching the serving layer directly
    // rather than pretending to be a caller.
    //
    // Wrapped, because that used to be crow's job. app.handle_full() turned an
    // escaping exception into a 500 on a response nobody read; here an escape
    // would unwind through workerLoop() out of the thread function and
    // std::terminate the process - a failing warm-up query would kill the
    // server. One endpoint's bad template must not stop the others being
    // warmed, or the scheduler below from running at all.
    try {
        api_server.warmEndpoint(endpoint);
    } catch (const std::exception& ex) {
        CROW_LOG_ERROR << "Heartbeat failed for " << endpoint.urlPath << ": " << ex.what();
    } catch (...) {
        CROW_LOG_ERROR << "Heartbeat failed for " << endpoint.urlPath
                       << ": non-standard exception";
    }
}

std::chrono::system_clock::time_point HeartbeatWorker::seedLastRefresh(const EndpointConfig& endpoint) {
    // The first time a cache is seen, its "last refresh" is the last REAL one: the
    // warm-up this process just ran, else the newest snapshot of the table (a
    // previous process's refresh), else this process's start.
    //
    // It used to be "never", so every scheduled cache refreshed on the first scan
    // after boot: on top of `warmup: always` (built twice) and straight through
    // `warmup: if-empty` / `never`, which exist to avoid exactly that.
    auto cache_manager = api_server.getCacheManager();
    if (cache_manager) {
        if (const auto warmed = cache_manager->lastRefreshCompletedAt(config_manager, endpoint)) {
            return *warmed;
        }
        if (const auto snapshot = cache_manager->newestSnapshotTime(config_manager, endpoint)) {
            return *snapshot;
        }
    }
    return started_at;
}

void HeartbeatWorker::performDuckLakeScheduledTasks() {
    const auto& ducklakeConfig = config_manager->getDuckLakeConfig();
    if (!ShouldRunCacheMaintenance(ducklakeConfig)) {
        return;
    }
    auto cache_manager = api_server.getCacheManager();
    if (!cache_manager || cache_manager->shutdownRequested()) {
        return;
    }

    std::set<std::string> scanned;
    const auto endpoints = config_manager->getEndpoints();
    for (const auto& endpoint : *endpoints) {
        if (!running) {
            return;
        }
        if (!isScheduledCache(endpoint)) {
            continue;
        }
        const std::string key = cacheTableKey(ducklakeConfig, endpoint);
        if (!scanned.insert(key).second) {
            continue;   // another endpoint on the same table already decided this scan
        }

        // Warm-up has not finished with this table yet; refreshing it now would only
        // be a duplicate of the build in progress.
        if (cache_manager->getEndpointReadiness(config_manager, endpoint).state ==
            CacheManager::ReadinessState::Starting) {
            continue;
        }

        // The WHOLE per-endpoint step is guarded: an exception that escapes this worker
        // thread std::terminate()s the process, and a bad schedule used to do exactly that
        // from shouldRunScheduledRefresh() on the second scan (#193).
        try {
            auto it = last_refresh_times.find(key);
            if (it == last_refresh_times.end()) {
                it = last_refresh_times.emplace(key, seedLastRefresh(endpoint)).first;
            }
            // A refresh made by anyone else (the config service, a write endpoint's
            // refresh-on-write) counts too.
            if (const auto other = cache_manager->lastRefreshCompletedAt(config_manager, endpoint)) {
                it->second = std::max(it->second, *other);
            }

            const auto now = std::chrono::system_clock::now();
            if (!RefreshDue(it->second, endpoint.cache.getRefreshTimeInSeconds(), now)) {
                continue;
            }
            CROW_LOG_INFO << "Running scheduled cache refresh for endpoint: " << endpoint.urlPath;
            try {
                std::map<std::string, std::string> params;
                cache_manager->refreshCache(config_manager, endpoint, params);
            } catch (const std::exception& ex) {
                // refreshCache() has already logged and marked the failure.
                CROW_LOG_DEBUG << "Scheduled cache refresh for " << endpoint.urlPath << " failed: " << ex.what();
            }
            // Counted on failure too: without it the endpoint was due again on EVERY
            // scan, hammering a source that is down. The next try is one interval away.
            it->second = now;
        } catch (const std::exception& ex) {
            CROW_LOG_ERROR << "Skipping scheduled refresh for " << endpoint.urlPath << ": " << ex.what();
        }
    }

    // Compaction, on its OWN schedule. `compaction.schedule` used to be checked only
    // for presence (every 24 h whatever it said) and ran once per cached endpoint.
    try {
        if (ducklakeConfig.compaction.enabled && ducklakeConfig.compaction.schedule) {
            const auto interval = TimeInterval::parseSchedule(*ducklakeConfig.compaction.schedule);
            if (interval) {
                if (!last_compaction_time) {
                    last_compaction_time = cache_manager->lastCompactionTime(config_manager).value_or(started_at);
                }
                const auto now = std::chrono::system_clock::now();
                if (RefreshDue(last_compaction_time, *interval, now)) {
                    cache_manager->performCompaction(config_manager);
                    last_compaction_time = now;
                }
            }
        }
    } catch (const std::exception& ex) {
        CROW_LOG_ERROR << "DuckLake compaction failed: " << ex.what();
    }
}

bool HeartbeatWorker::ShouldRunCacheMaintenance(const DuckLakeConfig& ducklake_config) {
    return ducklake_config.enabled && ducklake_config.scheduler.enabled && !ducklake_config.read_only;
}

} // namespace flapi
