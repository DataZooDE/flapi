#include "mcp_task_manager.hpp"

#include <iomanip>
#include <random>
#include <sstream>

namespace flapi {

MCPTaskManager::MCPTaskManager(size_t workers, size_t queue_depth, SqlExec sql_exec)
    : sql_exec_(std::move(sql_exec)), queue_depth_(queue_depth) {
    if (workers == 0) {
        workers = 1;
    }
    recoverTasks();  // no-op without persistence
    for (size_t i = 0; i < workers; ++i) {
        workers_.emplace_back([this] { workerLoop(); });
    }
}

namespace {
// DuckDB single-quote escaping for string literals.
std::string sqlLit(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') {
            out += "''";
        } else {
            out += c;
        }
    }
    out += "'";
    return out;
}

std::string statusToString(MCPTaskManager::Status s) {
    switch (s) {
        case MCPTaskManager::Status::Working:   return "working";
        case MCPTaskManager::Status::Completed: return "completed";
        case MCPTaskManager::Status::Failed:    return "failed";
        case MCPTaskManager::Status::Cancelled: return "cancelled";
    }
    return "working";
}

MCPTaskManager::Status statusFromString(const std::string& s) {
    if (s == "completed") {
        return MCPTaskManager::Status::Completed;
    }
    if (s == "failed") {
        return MCPTaskManager::Status::Failed;
    }
    if (s == "cancelled") {
        return MCPTaskManager::Status::Cancelled;
    }
    return MCPTaskManager::Status::Working;
}
} // namespace

void MCPTaskManager::persistTask(const Task& t) {
    if (!sql_exec_) {
        return;
    }
    try {
        // Upsert the task row. created_at is stored as epoch millis so recovery
        // can reconstruct the steady-clock offset approximately (TTL uses it
        // only for terminal-task reaping, so wall-clock is acceptable here).
        const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::string sql =
            "INSERT OR REPLACE INTO flapi_mcp_tasks "
            "(task_id, tool_name, principal, status, updated_at_ms, ttl_ms, "
            " poll_interval_ms, result_json, error_message) VALUES ("
            + sqlLit(t.task_id) + "," + sqlLit(t.tool_name) + "," + sqlLit(t.principal) + ","
            + sqlLit(statusToString(t.status)) + "," + std::to_string(now_ms) + ","
            + std::to_string(t.ttl_ms) + "," + std::to_string(t.poll_interval_ms) + ","
            + sqlLit(t.result_json) + "," + sqlLit(t.error_message) + ")";
        sql_exec_(sql);
    } catch (const std::exception& e) {
        // Persistence is best-effort; a DB hiccup must not break task execution.
        // (Durability is lost for this write, but the in-memory task is intact.)
    }
}

void MCPTaskManager::recoverTasks() {
    if (!sql_exec_) {
        return;
    }
    try {
        sql_exec_(
            "CREATE TABLE IF NOT EXISTS flapi_mcp_tasks ("
            "task_id VARCHAR PRIMARY KEY, tool_name VARCHAR, principal VARCHAR, "
            "status VARCHAR, updated_at_ms BIGINT, ttl_ms BIGINT, "
            "poll_interval_ms BIGINT, result_json VARCHAR, error_message VARCHAR)");
        // Any task still 'working' was interrupted by the previous process exit.
        sql_exec_(
            "UPDATE flapi_mcp_tasks SET status='failed', "
            "error_message='server restarted while task was running' "
            "WHERE status='working'");
        auto rows = sql_exec_("SELECT task_id, tool_name, principal, status, ttl_ms, "
                              "poll_interval_ms, result_json, error_message, updated_at_ms "
                              "FROM flapi_mcp_tasks");
        const auto now_wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::lock_guard<std::mutex> lock(mu_);
        for (const auto& r : rows) {
            auto entry = std::make_shared<Entry>();
            auto get = [&r](const char* k) -> std::string {
                auto it = r.find(k);
                return it == r.end() ? std::string() : it->second;
            };
            entry->task.task_id = get("task_id");
            entry->task.tool_name = get("tool_name");
            entry->task.principal = get("principal");
            entry->task.status = statusFromString(get("status"));
            // Reconstruct created_at so the recovered task keeps its ORIGINAL age
            // (steady_clock can't be persisted, so age it by wall-clock elapsed
            // since the last persisted update) — otherwise recovery would reset
            // every task's TTL window.
            const int64_t updated_ms = std::atoll(get("updated_at_ms").c_str());
            const int64_t elapsed_ms = (updated_ms > 0 && now_wall_ms > updated_ms)
                ? (now_wall_ms - updated_ms) : 0;
            entry->task.created_at = std::chrono::steady_clock::now()
                - std::chrono::milliseconds(elapsed_ms);
            entry->task.ttl_ms = std::atoll(get("ttl_ms").c_str());
            entry->task.poll_interval_ms = std::atoll(get("poll_interval_ms").c_str());
            entry->task.result_json = get("result_json");
            entry->task.error_message = get("error_message");
            if (!entry->task.task_id.empty()) {
                // Recovered tasks are terminal (working ones were failed above);
                // they are queryable via tasks/get but not re-run.
                tasks_[entry->task.task_id] = entry;
            }
        }
    } catch (const std::exception& e) {
        // If recovery fails (e.g. schema drift or the DB is not ready), continue
        // with an empty store — task durability is best-effort.
        fprintf(stderr, "[flapi] MCP task recovery skipped: %s\n", e.what());
    }
}

MCPTaskManager::~MCPTaskManager() {
    shutdown();
}

std::string MCPTaskManager::newTaskId() {
    // thread_local so concurrent submit() calls never race on the RNG.
    thread_local std::mt19937_64 rng{std::random_device{}()};
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
    persistTask(entry->task);  // durable record before we return the id
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
            {
                std::unique_lock<std::mutex> lock(mu_);
                if (entry->task.status == Status::Working) {
                    entry->task.status = Status::Cancelled;
                }
            }
            persistTask(entry->task);
            continue;
        }

        std::string result;
        std::string error;
        try {
            result = entry->work(entry->cancelled);
        } catch (const std::exception& e) {
            error = e.what();
        }

        {
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
        persistTask(entry->task);  // durable terminal state
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
    const auto& t = it->second->task;
    // Enforce TTL on read too (not only during submit's sweep): a terminal task
    // past its TTL must not keep returning potentially sensitive results on an
    // otherwise idle server.
    if (t.ttl_ms > 0 && t.status != Status::Working) {
        const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - t.created_at).count();
        if (age > t.ttl_ms) {
            found = false;
            return false;
        }
    }
    found = true;
    // A taskId is a name, not a capability: re-check ownership on every poll.
    if (t.principal != principal) {
        return false;
    }
    out = t;
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
