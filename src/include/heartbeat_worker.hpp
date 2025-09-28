#pragma once

#include <crow.h>
#include <thread>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

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

private:
    std::shared_ptr<ConfigManager> config_manager;
    APIServer& api_server;
    std::thread worker_thread;
    std::atomic<bool> running;
    std::condition_variable cv;
    std::mutex mutex;
    
    // DuckLake scheduler state
    std::map<std::string, std::chrono::system_clock::time_point> last_refresh_times;
    std::chrono::system_clock::time_point last_compaction_time;

    void workerLoop();
    void performHeartbeat(const EndpointConfig& endpoint);
    
    // DuckLake scheduler functionality
    void performDuckLakeScheduledTasks();
    bool shouldRunScheduledRefresh(const EndpointConfig& endpoint, std::chrono::system_clock::time_point now);
    bool shouldRunCompaction(std::chrono::system_clock::time_point now);
    void performDuckLakeCompaction();
};

} // namespace flapi
