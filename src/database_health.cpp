#include "database_health.hpp"

#include <atomic>
#include <mutex>

#include <crow.h>

namespace flapi {

namespace {
std::atomic<bool> g_fatal{false};
std::mutex g_reason_mutex;
std::string g_reason;

// Long DuckDB errors quote the statement; the reason is for an operator's
// probe output, not a dump. /health is unauthenticated.
constexpr std::size_t kMaxReasonLength = 300;
}  // namespace

bool DatabaseHealth::IsInvalidationError(const std::string& error_message) {
    return error_message.find("database has been invalidated") != std::string::npos;
}

void DatabaseHealth::NoteDatabaseError(const std::string& error_message) {
    if (g_fatal.load(std::memory_order_acquire) || !IsInvalidationError(error_message)) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_reason_mutex);
        if (g_fatal.load(std::memory_order_relaxed)) {
            return;
        }
        g_reason = error_message.size() > kMaxReasonLength ? error_message.substr(0, kMaxReasonLength) + "..."
                                                           : error_message;
        g_fatal.store(true, std::memory_order_release);
    }
    CROW_LOG_CRITICAL << "FATAL: the DuckDB database has been invalidated and cannot be used until flapi "
                         "restarts; /health and /health/live now answer 503 so a supervisor restarts the "
                         "process. Cause: "
                      << error_message;
}

bool DatabaseHealth::IsFatal() {
    return g_fatal.load(std::memory_order_acquire);
}

std::optional<std::string> DatabaseHealth::FatalReason() {
    if (!IsFatal()) {
        return std::nullopt;
    }
    std::lock_guard<std::mutex> lock(g_reason_mutex);
    return g_reason;
}

std::string DatabaseHealth::FatalMessage() {
    return "The database has been invalidated by an earlier internal error and flapi must be restarted; "
           "GET /health shows the cause.";
}

std::string DatabaseHealth::FatalResponseJson() {
    crow::json::wvalue body;
    body["error"] = "database_invalidated";
    body["message"] = FatalMessage();
    return body.dump();
}

void DatabaseHealth::ResetForTesting() {
    std::lock_guard<std::mutex> lock(g_reason_mutex);
    g_reason.clear();
    g_fatal.store(false, std::memory_order_release);
}

}  // namespace flapi
