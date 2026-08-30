#include "mcp_task_manager.hpp"

#include <iomanip>
#include <random>
#include <sstream>

namespace flapi {

MCPTaskManager::MCPTaskManager(size_t workers, size_t queue_depth)
    : queue_depth_(queue_depth) {
    if (workers == 0) {
        workers = 1;
    }
    for (size_t i = 0; i < workers; ++i) {
        workers_.emplace_back([this] { workerLoop(); });
    }
}

MCPTaskManager::~MCPTaskManager() {
    shutdown();
}

std::string MCPTaskManager::newTaskId() {
    static std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<uint64_t> dist;
    std::ostringstream oss;
    oss << std::hex << std::setw(16) << std::setfill('0') << dist(rng)
        << std::setw(16) << std::setfill('0') << dist(rng);
    return "task_" + oss.str();
}

void MCPTaskManager::sweepExpiredLocked() {
    const auto now = std::chrono::steady_clock::now();
    for (auto it = tasks_.begin(); it != tasks_.end();) {
        const auto& t = it->second->task;
        if (t.ttl_ms > 0) {
            const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 now - t.created_at).count();
            // Reap terminal tasks past their TTL.
            const bool terminal = t.status != Status::Working;
            if (terminal && age > t.ttl_ms) {
                it = tasks_.erase(it);
                continue;
            }
        }
        ++it;
    }
}

std::string MCPTaskManager::submit(const std::string& tool_name, const std::string& principal,
                                   int64_t ttl_ms, int64_t poll_interval_ms, Work work) {
    auto entry = std::make_shared<Entry>();
    entry->task.task_id = newTaskId();
    entry->task.tool_name = tool_name;
    entry->task.principal = principal;
    entry->task.status = Status::Working;
    entry->task.created_at = std::chrono::steady_clock::now();
    entry->task.ttl_ms = ttl_ms;
    entry->task.poll_interval_ms = poll_interval_ms;
    entry->work = std::move(work);

    {
        std::unique_lock<std::mutex> lock(mu_);
        sweepExpiredLocked();
        if (queue_.size() >= queue_depth_) {
            return "";  // backpressure
        }
        tasks_[entry->task.task_id] = entry;
        queue_.push_back(entry);
    }
    cv_.notify_one();
    return entry->task.task_id;
}

void MCPTaskManager::workerLoop() {
    for (;;) {
        std::shared_ptr<Entry> entry;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_ && queue_.empty()) {
                return;
            }
            entry = queue_.front();
            queue_.pop_front();
        }

        // Skip work already cancelled before it started.
        if (entry->cancelled.load()) {
            std::unique_lock<std::mutex> lock(mu_);
            if (entry->task.status == Status::Working) {
                entry->task.status = Status::Cancelled;
            }
            continue;
        }

        std::string result;
        std::string error;
        try {
            result = entry->work(entry->cancelled);
        } catch (const std::exception& e) {
            error = e.what();
        }

        std::unique_lock<std::mutex> lock(mu_);
        if (entry->cancelled.load()) {
            entry->task.status = Status::Cancelled;
        } else if (!error.empty()) {
            entry->task.status = Status::Failed;
            entry->task.error_message = error;
        } else {
            entry->task.status = Status::Completed;
            entry->task.result_json = std::move(result);
        }
    }
}

bool MCPTaskManager::get(const std::string& task_id, const std::string& principal,
                         Task& out, bool& found) const {
    std::unique_lock<std::mutex> lock(mu_);
    auto it = tasks_.find(task_id);
    if (it == tasks_.end()) {
        found = false;
        return false;
    }
    found = true;
    // A taskId is a name, not a capability: re-check ownership on every poll.
    if (it->second->task.principal != principal) {
        return false;
    }
    out = it->second->task;
    return true;
}

bool MCPTaskManager::cancel(const std::string& task_id, const std::string& principal) {
    std::unique_lock<std::mutex> lock(mu_);
    auto it = tasks_.find(task_id);
    if (it == tasks_.end() || it->second->task.principal != principal) {
        return false;
    }
    it->second->cancelled.store(true);
    // If it has not started yet, mark it cancelled immediately; a running task
    // is marked cancelled by the worker when its work returns.
    if (it->second->task.status == Status::Working) {
        // Leave running tasks Working until the worker observes cancellation;
        // queued-but-not-started tasks are flipped by the worker's pre-check.
    }
    return true;
}

void MCPTaskManager::shutdown() {
    {
        std::unique_lock<std::mutex> lock(mu_);
        if (stopping_) {
            return;
        }
        stopping_ = true;
        // Signal cancellation to everything so in-flight work can bail out.
        for (auto& [id, entry] : tasks_) {
            entry->cancelled.store(true);
        }
    }
    cv_.notify_all();
    for (auto& t : workers_) {
        if (t.joinable()) {
            t.join();
        }
    }
}

} // namespace flapi
