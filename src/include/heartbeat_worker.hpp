#pragma once

#include <crow.h>
#include <thread>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <vector>

#include "api_server.hpp"
#include "config_manager.hpp"

namespace flapi {

class APIServer; // forward declaration
class HeartbeatWorker {
public:
    HeartbeatWorker(std::shared_ptr<ConfigManager> config_manager, APIServer& api_server);
    ~HeartbeatWorker();

    void start();
    void stop();

    // Scheduled refresh, retention and compaction write to the catalog. A read-only
    // replica must do none of them, and `ducklake.scheduler.enabled: false` written in
    // the config opts out; otherwise they run (a `cache.schedule` is the opt-in).
    static bool ShouldRunCacheMaintenance(const DuckLakeConfig& ducklake_config);

    // How often cache schedules are checked: `ducklake.scheduler.scan-interval` when
    // set, else min(heartbeat.worker-interval, the shortest cache.schedule), never
    // below one second.
    static std::chrono::seconds CacheScanInterval(const DuckLakeConfig& ducklake_config,
                                                  const GlobalHeartbeatConfig& heartbeat_config,
                                                  const std::vector<EndpointConfig>& endpoints);

    // Whether a refresh on `interval` is due at `now`, given when the cache was last
    // refreshed. Pure, for tests.
    static bool RefreshDue(std::optional<std::chrono::system_clock::time_point> last_refresh,
                           std::chrono::seconds interval,
                           std::chrono::system_clock::time_point now);

private:
    std::shared_ptr<ConfigManager> config_manager;
    APIServer& api_server;
    std::thread worker_thread;
    std::atomic<bool> running;
    std::condition_variable cv;
    std::mutex mutex;

    // DuckLake scheduler state, keyed by cache table (catalog.schema.table): two
    // endpoints sharing a table share one schedule.
    std::map<std::string, std::chrono::system_clock::time_point> last_refresh_times;
    std::optional<std::chrono::system_clock::time_point> last_compaction_time;
    std::chrono::system_clock::time_point started_at;

    void workerLoop();
    void logSchedulerSetup(std::chrono::seconds scan_interval);
    void performHeartbeat(const EndpointConfig& endpoint);

    // DuckLake scheduler functionality
    void performDuckLakeScheduledTasks();
    std::chrono::system_clock::time_point seedLastRefresh(const EndpointConfig& endpoint);
};

} // namespace flapi
