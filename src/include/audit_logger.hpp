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
    // W3C ids, empty when tracing is inactive. Emitted only when non-empty, so
    // the audit schema is unchanged for operators who never enable tracing.
    std::string trace_id;                 // 32 hex chars
    std::string span_id;                  // 16 hex chars
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

    // Exposed for tests: asserting on the exact serialized line is the only way
    // to prove a field is absent rather than merely empty.
    std::string serialiseEventForTest(const AuditEvent& event) const { return serialiseEvent(event); }

private:
    AuditConfig config_;
    std::mutex write_mutex_;
    std::unique_ptr<std::ostream> file_stream_;
    std::ostream* sink_stream_ = nullptr;  // non-owning view onto the active sink

    static std::string nowIso8601();
    static std::string generateRequestId();
    std::string serialiseEvent(const AuditEvent& event) const;
};

// Build an audit event from the request context, so the audit line, the log
// lines and the trace all carry one identity and one latency. Callers override
// the few fields the context cannot know (an MCP tool name, say).
//
// Keeping AuditLogger::generateRequestId as a fallback: the audit log must keep
// working when no context is ambient.
struct RequestContext;
AuditEvent auditEventFrom(const RequestContext& rc);

} // namespace flapi
