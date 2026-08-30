#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>

#include "mcp_task_manager.hpp"

namespace flapi {
namespace test {

using Status = MCPTaskManager::Status;

namespace {
bool waitFor(MCPTaskManager& m, const std::string& id, const std::string& principal,
             Status want, int timeout_ms = 2000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        MCPTaskManager::Task t;
        bool found = false;
        if (m.get(id, principal, t, found) && t.status == want) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}
}

TEST_CASE("MCPTaskManager: a submitted task runs and completes", "[mcp][tasks]") {
    MCPTaskManager m(2, 16);
    auto id = m.submit("tool", "alice", 60000, 100,
                       [](const std::atomic<bool>&) { return std::string("{\"ok\":true}"); });
    REQUIRE_FALSE(id.empty());
    REQUIRE(waitFor(m, id, "alice", Status::Completed));

    MCPTaskManager::Task t;
    bool found = false;
    REQUIRE(m.get(id, "alice", t, found));
    REQUIRE(t.result_json == "{\"ok\":true}");
}

TEST_CASE("MCPTaskManager: a throwing work marks the task failed", "[mcp][tasks]") {
    MCPTaskManager m(1, 16);
    auto id = m.submit("tool", "alice", 60000, 100,
                       [](const std::atomic<bool>&) -> std::string { throw std::runtime_error("boom"); });
    REQUIRE(waitFor(m, id, "alice", Status::Failed));
    MCPTaskManager::Task t;
    bool found = false;
    REQUIRE(m.get(id, "alice", t, found));
    REQUIRE(t.error_message == "boom");
}

TEST_CASE("MCPTaskManager: get re-checks principal ownership", "[mcp][tasks]") {
    MCPTaskManager m(1, 16);
    auto id = m.submit("tool", "alice", 60000, 100,
                       [](const std::atomic<bool>&) { return std::string("{}"); });
    REQUIRE(waitFor(m, id, "alice", Status::Completed));

    MCPTaskManager::Task t;
    bool found = false;
    // Wrong principal: not returned, but the task does exist.
    REQUIRE_FALSE(m.get(id, "mallory", t, found));
    REQUIRE(found);
    // Unknown id: not found at all.
    REQUIRE_FALSE(m.get("task_missing", "alice", t, found));
    REQUIRE_FALSE(found);
}

TEST_CASE("MCPTaskManager: cancel is honoured and cross-principal cancel is refused", "[mcp][tasks]") {
    MCPTaskManager m(1, 16);
    std::atomic<bool> release{false};
    // Work blocks until cancelled or released so we can cancel it mid-flight.
    auto id = m.submit("tool", "alice", 60000, 100,
                       [&release](const std::atomic<bool>& cancelled) {
                           for (int i = 0; i < 200; ++i) {
                               if (cancelled.load() || release.load()) break;
                               std::this_thread::sleep_for(std::chrono::milliseconds(5));
                           }
                           return std::string("{}");
                       });

    REQUIRE_FALSE(m.cancel(id, "mallory"));   // wrong principal
    REQUIRE(m.cancel(id, "alice"));           // owner
    REQUIRE(waitFor(m, id, "alice", Status::Cancelled, 3000));
}

TEST_CASE("MCPTaskManager: queue backpressure returns an empty id", "[mcp][tasks]") {
    // One worker, queue depth 1. Fill the worker with a blocking task, then the
    // queue with one, so a third submit is rejected.
    MCPTaskManager m(1, 1);
    std::atomic<bool> release{false};
    auto blocker = [&release](const std::atomic<bool>& cancelled) {
        while (!release.load() && !cancelled.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return std::string("{}");
    };
    auto id1 = m.submit("t", "a", 60000, 100, blocker);  // taken by the worker
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto id2 = m.submit("t", "a", 60000, 100, blocker);  // queued
    auto id3 = m.submit("t", "a", 60000, 100, blocker);  // rejected
    REQUIRE_FALSE(id1.empty());
    REQUIRE_FALSE(id2.empty());
    REQUIRE(id3.empty());
    release.store(true);
}

} // namespace test
} // namespace flapi
