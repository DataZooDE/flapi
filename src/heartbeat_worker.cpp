#include "heartbeat_worker.hpp"

namespace flapi {

HeartbeatWorker::HeartbeatWorker(std::shared_ptr<ConfigManager> config_manager, APIServer& api_server)
    : config_manager(config_manager), api_server(api_server), running(false) {}

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

void HeartbeatWorker::workerLoop() {
    while (running) {
        if (config_manager->getGlobalHeartbeatConfig().enabled) 
        {    
            for (const auto& endpoint : config_manager->getEndpoints()) {
                if (!running) break; // Check if we should exit
                if (endpoint.heartbeat.enabled) {
                    performHeartbeat(endpoint);
                }
            }
        }

        // DuckLake scheduler functionality
        performDuckLakeScheduledTasks();

        // Use condition variable to wait with timeout
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait_for(lock, 
                   config_manager->getGlobalHeartbeatConfig().workerInterval,
                   [this] { return !running; });
    }
}

void HeartbeatWorker::performHeartbeat(const EndpointConfig& endpoint) 
{
    CROW_LOG_DEBUG << "Performing heartbeat for endpoint " << endpoint.urlPath;
    api_server.requestForEndpoint(endpoint);
}

void HeartbeatWorker::performDuckLakeScheduledTasks() {
    const auto& ducklakeConfig = config_manager->getDuckLakeConfig();
    if (!ducklakeConfig.enabled || !ducklakeConfig.scheduler.enabled) {
        return;
    }

    // Check if it's time to run scheduled cache refreshes
    auto now = std::chrono::system_clock::now();
    
    for (const auto& endpoint : config_manager->getEndpoints()) {
        if (!endpoint.cache.enabled || endpoint.cache.table.empty()) {
            continue;
        }

        if (!endpoint.cache.schedule) {
            continue; // No schedule configured
        }

        // Simple schedule check - in production, this would use cron-like scheduling
        if (shouldRunScheduledRefresh(endpoint, now)) {
            try {
                CROW_LOG_INFO << "Running scheduled cache refresh for endpoint: " << endpoint.urlPath;
                
                // Trigger cache refresh via API server
                std::map<std::string, std::string> params;
                api_server.getCacheManager()->refreshCache(config_manager, endpoint, params);
                
                // Update last run time
                last_refresh_times[endpoint.urlPath] = now;
                
            } catch (const std::exception& ex) {
                CROW_LOG_ERROR << "Failed scheduled cache refresh for " << endpoint.urlPath << ": " << ex.what();
            }
        }
    }

    // Run DuckLake compaction if scheduled
    if (ducklakeConfig.compaction.enabled && ducklakeConfig.compaction.schedule) {
        if (shouldRunCompaction(now)) {
            performDuckLakeCompaction();
        }
    }
}

bool HeartbeatWorker::shouldRunScheduledRefresh(const EndpointConfig& endpoint, std::chrono::system_clock::time_point now) {
    if (!endpoint.cache.schedule) {
        return false;
    }

    auto lastRunIt = last_refresh_times.find(endpoint.urlPath);
    if (lastRunIt == last_refresh_times.end()) {
        // Never run before, so run now
        return true;
    }

    auto refreshInterval = endpoint.cache.getRefreshTimeInSeconds();
    auto timeSinceLastRun = std::chrono::duration_cast<std::chrono::seconds>(now - lastRunIt->second);
    
    return timeSinceLastRun >= refreshInterval;
}

bool HeartbeatWorker::shouldRunCompaction(std::chrono::system_clock::time_point now) {
    // Simple daily compaction check - in production, this would parse cron expressions
    auto timeSinceLastCompaction = std::chrono::duration_cast<std::chrono::hours>(now - last_compaction_time);
    return timeSinceLastCompaction >= std::chrono::hours(24);
}

void HeartbeatWorker::performDuckLakeCompaction() {
    const auto& ducklakeConfig = config_manager->getDuckLakeConfig();
    const std::string catalog = ducklakeConfig.alias;
    
    try {
        CROW_LOG_INFO << "Running DuckLake compaction for catalog: " << catalog;
        
        // Get database manager to run compaction
        auto db_manager = api_server.getDatabaseManager();
        
        // Run compaction on all cached tables
        for (const auto& endpoint : config_manager->getEndpoints()) {
            if (!endpoint.cache.enabled || endpoint.cache.table.empty()) {
                continue;
            }
            
            const std::string schema = endpoint.cache.schema.empty() ? "main" : endpoint.cache.schema;
            const std::string table = endpoint.cache.table;
            
            std::string compactionSQL = "CALL ducklake_merge_adjacent_files('" + catalog + "')";
            std::map<std::string, std::string> params;
            
            try {
                db_manager->executeDuckLakeQuery(compactionSQL, params);
                CROW_LOG_DEBUG << "Merged adjacent files for catalog " << catalog;
            } catch (const std::exception& ex) {
                CROW_LOG_WARNING << "Failed to merge adjacent files for catalog " << catalog << ": " << ex.what();
            }
        }
        
        last_compaction_time = std::chrono::system_clock::now();
        CROW_LOG_INFO << "DuckLake file merging completed";
        
    } catch (const std::exception& ex) {
        CROW_LOG_ERROR << "DuckLake compaction failed: " << ex.what();
    }
}

} // namespace flapi
