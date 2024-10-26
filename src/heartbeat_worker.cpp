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
                if (endpoint.heartbeat.enabled) {
                    performHeartbeat(endpoint);
                }
            }

        }

        std::this_thread::sleep_for(config_manager->getGlobalHeartbeatConfig().workerInterval);
    }
}

void HeartbeatWorker::performHeartbeat(const EndpointConfig& endpoint) 
{
    CROW_LOG_DEBUG << "Performing heartbeat for endpoint " << endpoint.urlPath;
    api_server.requestForEndpoint(endpoint);
}

} // namespace flapi
