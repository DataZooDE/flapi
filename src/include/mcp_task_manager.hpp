#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace flapi {

// In-memory store + bounded worker pool for the MCP 2026-07-28 Tasks extension.
//
// A tool configured `async` (or one that outruns its `async-after-ms` budget)
// is submitted here: the caller gets a taskId immediately and polls tasks/get
// until the task reaches a terminal state. This keeps multi-minute analytical
// queries off the request connection.
//
// NOTE: this store is in-memory only. Tasks do NOT survive a process restart
// (a restart-durable DuckLake-backed store is a planned follow-up). The public
// API is deliberately shaped so that store can be swapped in behind it.
class MCPTaskManager {
public:
    enum class Status { Working, Completed, Failed, Cancelled };

    struct Task {
        std::string task_id;
        std::string tool_name;
        std::string principal;   // owner; tasks/get is re-checked against this
        Status status = Status::Working;
        std::chrono::steady_clock::time_point created_at;
        int64_t ttl_ms = 0;
        int64_t poll_interval_ms = 0;
        std::string result_json;  // MCP tool result envelope, when Completed
        std::string error_message; // when Failed
    };

    // A running task registers an interrupt closure through this callback so the
    // manager can stop it preemptively (issue #111). The closure is invoked from
    // another thread by cancel()/shutdown(); it must be safe to call concurrently
    // with the work and after the work has finished (a no-op then). Typically it
    // interrupts the DuckDB query executing on the worker thread.
    using SetInterrupt = std::function<void(std::function<void()>)>;

    // The unit of work a submitted task runs: it returns the serialized MCP
    // tool result envelope (the same string a synchronous tools/call produces).
    // It should honour `cancelled` cooperatively where possible, and may call
    // `set_interrupt` once it has a preemptive cancellation hook to expose.
    using Work = std::function<std::string(const std::atomic<bool>& cancelled,
                                           const SetInterrupt& set_interrupt)>;

    // Optional durability: a callback that runs a SQL statement and returns its
    // rows (each a column->string map). When provided, tasks are persisted to a
    // `flapi_mcp_tasks` table on creation and on every terminal transition, and
    // recovered on construction (a task left `working` by a crash is marked
    // `failed`). Durability is real only when the underlying DuckDB is
    // file-backed (duckdb.db_path); with an in-memory database the table is
    // recreated empty each start. A null exec disables persistence entirely.
    using SqlExec = std::function<std::vector<std::map<std::string, std::string>>(const std::string&)>;

    MCPTaskManager(size_t workers, size_t queue_depth, SqlExec sql_exec = nullptr);
    ~MCPTaskManager();

    // Submit work; returns the new task_id, or empty string if the queue is
    // full (backpressure — the caller should fall back to synchronous or error).
    std::string submit(const std::string& tool_name, const std::string& principal,
                       int64_t ttl_ms, int64_t poll_interval_ms, Work work);

    // Snapshot a task if it exists and belongs to `principal`. `found` is set
    // true when a task with that id exists at all (even if owned by another
    // principal), so the caller can distinguish not-found from not-authorized.
    bool get(const std::string& task_id, const std::string& principal,
             Task& out, bool& found) const;

    // Cooperatively cancel a task the principal owns. Returns true if a task was
    // found and moved toward cancellation.
    bool cancel(const std::string& task_id, const std::string& principal);

    void shutdown();

private:
    struct Entry {
        Task task;
        std::atomic<bool> cancelled{false};
        Work work;
        // Preemptive-cancel hook, set by the running work and cleared when it
        // finishes; guarded by the manager mutex (mu_). Empty when the task is
        // queued or already terminal.
        std::function<void()> interrupt;
    };

    void workerLoop();
    void sweepExpiredLocked();
    static std::string newTaskId();

    // Persistence helpers (no-ops when sql_exec_ is null).
    void persistTask(const Task& t);
    void recoverTasks();

    SqlExec sql_exec_;
    size_t queue_depth_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::unordered_map<std::string, std::shared_ptr<Entry>> tasks_;
    std::deque<std::shared_ptr<Entry>> queue_;
    std::vector<std::thread> workers_;
    bool stopping_ = false;
};

} // namespace flapi
