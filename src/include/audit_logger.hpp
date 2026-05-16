#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <ostream>
#include <string>
#include <unordered_set>

namespace flapi {

struct AuditConfig {
    bool enabled = false;
    std::string sink = "stdout";          // "stdout" | "file" | "null"
    std::string path;                     // used when sink == "file"
    std::unordered_set<std::string> redact_keys;  // params with these keys are masked
};

struct AuditEvent {
    std::string timestamp;                // auto-filled if empty
    std::string request_id;               // auto-filled if empty
    std::string principal = "anonymous";  // username, or "anonymous" when unauthenticated
    std::string method;                   // "GET", "POST", "tools/call", etc.
    std::string target;                   // url path or tool name
    std::string status;                   // "success", "denied", "error:<code>" — free-form
    std::int64_t row_count = -1;          // -1 when not applicable (e.g. denial)
    std::int64_t latency_ms = -1;         // wall-clock elapsed
    std::map<std::string, std::string> params;  // already-redacted-by-caller is fine
};

// Append-only JSONL audit logger. Construct one per server; share by
// std::shared_ptr. The logger is thread-safe — call sites can race
// without coordination.
//
// Lifecycle is owned by whatever constructs it (typically the server
// bootstrap in main.cpp / APIServer). Writers SHOULD NOT bypass log()
// for any reason; redaction happens inside.
class AuditLogger {
public:
    explicit AuditLogger(AuditConfig config);
    ~AuditLogger();

    AuditLogger(const AuditLogger&) = delete;
    AuditLogger& operator=(const AuditLogger&) = delete;

    void log(AuditEvent event);
    bool isEnabled() const { return config_.enabled; }
    const AuditConfig& config() const { return config_; }

private:
    AuditConfig config_;
    std::mutex write_mutex_;
    std::unique_ptr<std::ostream> file_stream_;
    std::ostream* sink_stream_ = nullptr;  // non-owning view onto the active sink

    static std::string nowIso8601();
    static std::string generateRequestId();
    std::string serialiseEvent(const AuditEvent& event) const;
};

} // namespace flapi
