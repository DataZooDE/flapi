#pragma once

#include <crow.h>
#include <thread>
#include <atomic>
#include <chrono>

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

    void workerLoop();
    void performHeartbeat(const EndpointConfig& endpoint);
};

} // namespace flapi
