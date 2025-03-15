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

} // namespace flapi
