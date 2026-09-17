#include <catch2/catch_test_macros.hpp>
#include <crow/json.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

#include "audit_logger.hpp"
#include "request_context.hpp"

namespace flapi {
namespace test {

namespace {

namespace fs = std::filesystem;

class TempAuditFile {
public:
    TempAuditFile()
        : path_(fs::temp_directory_path() / ("flapi_audit_test_" +
                                              std::to_string(::rand()) + ".jsonl")) {}
    ~TempAuditFile() {
        if (fs::exists(path_)) {
            fs::remove(path_);
        }
    }
    fs::path path() const { return path_; }
    std::vector<std::string> readLines() const {
        std::vector<std::string> lines;
        std::ifstream f(path_);
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty()) {
                lines.push_back(line);
            }
        }
        return lines;
    }
private:
    fs::path path_;
};

AuditEvent sampleEvent() {
    AuditEvent ev;
    ev.principal = "alice";
    ev.target = "tools/call:customer_lookup";
    ev.method = "tools/call";
    ev.status = "success";
    ev.row_count = 7;
    ev.latency_ms = 12;
    ev.params = {{"id", "42"}, {"token", "secret123"}};
    return ev;
}

} // namespace

TEST_CASE("AuditLogger: disabled config is a no-op even when path is set",
          "[security][audit]") {
    TempAuditFile sink;
    AuditConfig cfg;
    cfg.enabled = false;
    cfg.sink = "file";
    cfg.path = sink.path().string();

    AuditLogger logger(cfg);
    logger.log(sampleEvent());

    // The file must not even be created when the logger is disabled.
    REQUIRE_FALSE(fs::exists(sink.path()));
}

TEST_CASE("AuditLogger: file sink emits one JSONL line per event",
          "[security][audit]") {
    TempAuditFile sink;
    AuditConfig cfg;
    cfg.enabled = true;
    cfg.sink = "file";
    cfg.path = sink.path().string();

    AuditLogger logger(cfg);
    logger.log(sampleEvent());
    logger.log(sampleEvent());

    auto lines = sink.readLines();
    REQUIRE(lines.size() == 2);

    auto parsed = crow::json::load(lines[0]);
    REQUIRE(parsed);
    REQUIRE(parsed["principal"].s() == std::string("alice"));
    REQUIRE(parsed["target"].s() == std::string("tools/call:customer_lookup"));
    REQUIRE(parsed["method"].s() == std::string("tools/call"));
    REQUIRE(parsed["status"].s() == std::string("success"));
    REQUIRE(parsed["row_count"].i() == 7);
    REQUIRE(parsed["latency_ms"].i() == 12);
}

TEST_CASE("AuditLogger: redact list masks listed param keys",
          "[security][audit]") {
    TempAuditFile sink;
    AuditConfig cfg;
    cfg.enabled = true;
    cfg.sink = "file";
    cfg.path = sink.path().string();
    cfg.redact_keys = {"token", "password"};

    AuditLogger logger(cfg);
    logger.log(sampleEvent());

    auto lines = sink.readLines();
    REQUIRE(lines.size() == 1);
    auto parsed = crow::json::load(lines[0]);
    REQUIRE(parsed);
    REQUIRE(parsed["params"]["id"].s() == std::string("42"));
    // The literal redaction marker must replace the secret value.
    REQUIRE(parsed["params"]["token"].s() == std::string("<redacted>"));
    // Original secret must not appear anywhere in the line.
    REQUIRE(lines[0].find("secret123") == std::string::npos);
}

TEST_CASE("AuditLogger: every event carries timestamp and request_id",
          "[security][audit]") {
    TempAuditFile sink;
    AuditConfig cfg;
    cfg.enabled = true;
    cfg.sink = "file";
    cfg.path = sink.path().string();

    AuditLogger logger(cfg);
    logger.log(sampleEvent());

    auto lines = sink.readLines();
    REQUIRE(lines.size() == 1);
    auto parsed = crow::json::load(lines[0]);
    REQUIRE(parsed);
    REQUIRE_FALSE(std::string(parsed["timestamp"].s()).empty());
    REQUIRE_FALSE(std::string(parsed["request_id"].s()).empty());
    // Timestamp must look ISO 8601-ish — at minimum start with four digits and a dash.
    const std::string ts = parsed["timestamp"].s();
    REQUIRE(ts.size() >= 5);
    REQUIRE(ts[4] == '-');
}

TEST_CASE("AuditLogger: explicit request_id is preserved",
          "[security][audit]") {
    TempAuditFile sink;
    AuditConfig cfg;
    cfg.enabled = true;
    cfg.sink = "file";
    cfg.path = sink.path().string();

    AuditLogger logger(cfg);
    auto ev = sampleEvent();
    ev.request_id = "req-deadbeef";
    logger.log(ev);

    auto lines = sink.readLines();
    REQUIRE(lines.size() == 1);
    auto parsed = crow::json::load(lines[0]);
    REQUIRE(parsed);
    REQUIRE(parsed["request_id"].s() == std::string("req-deadbeef"));
}

TEST_CASE("AuditLogger: concurrent writes produce well-formed JSONL",
          "[security][audit][threading]") {
    TempAuditFile sink;
    AuditConfig cfg;
    cfg.enabled = true;
    cfg.sink = "file";
    cfg.path = sink.path().string();

    AuditLogger logger(cfg);

    constexpr int kThreads = 8;
    constexpr int kEventsPerThread = 25;
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&logger, t]() {
            for (int i = 0; i < kEventsPerThread; ++i) {
                AuditEvent ev;
                ev.principal = "thread-" + std::to_string(t);
                ev.target = "tool-" + std::to_string(i);
                ev.method = "tools/call";
                ev.status = "success";
                ev.row_count = i;
                ev.latency_ms = 1;
                logger.log(ev);
            }
        });
    }
    for (auto& w : workers) {
        w.join();
    }

    auto lines = sink.readLines();
    REQUIRE(lines.size() == static_cast<size_t>(kThreads * kEventsPerThread));
    // Every line must be valid JSON in isolation; concurrent writes
    // must not interleave inside a single line.
    for (const auto& l : lines) {
        auto parsed = crow::json::load(l);
        REQUIRE(parsed);
        REQUIRE(parsed["method"].s() == std::string("tools/call"));
    }
}

TEST_CASE("AuditLogger: null sink is honoured (no I/O)",
          "[security][audit]") {
    // "null" sink is the no-op writer — useful for tests that exercise the
    // production code path without an audit file.
    AuditConfig cfg;
    cfg.enabled = true;
    cfg.sink = "null";
    AuditLogger logger(cfg);
    REQUIRE_NOTHROW(logger.log(sampleEvent()));
}

} // namespace test
} // namespace flapi

// --- Issue 1: correlation between the audit log and the request context -----

TEST_CASE("auditEventFrom carries the request context's identity", "[audit][request_context]") {
    flapi::RequestContext rc;
    flapi::RequestContext::mintRequestId(rc.request_id);
    rc.setTraceId("4bf92f3577b34da6a3ce929d0e0e4736");
    rc.setSpanId("00f067aa0ba902b7");
    rc.principal = "alice";
    rc.http_method = "GET";
    rc.route_template = "/customers/{id}";
    rc.row_count = 7;
    rc.status_code = 200;
    rc.t0 = std::chrono::steady_clock::now() - std::chrono::milliseconds(12);

    const flapi::AuditEvent ev = flapi::auditEventFrom(rc);

    REQUIRE(ev.request_id == std::string(rc.requestIdView()));
    REQUIRE(ev.trace_id == "4bf92f3577b34da6a3ce929d0e0e4736");
    REQUIRE(ev.span_id == "00f067aa0ba902b7");
    REQUIRE(ev.principal == "alice");
    REQUIRE(ev.method == "GET");
    REQUIRE(ev.target == "/customers/{id}");
    REQUIRE(ev.row_count == 7);
    REQUIRE(ev.latency_ms >= 10);
    REQUIRE(ev.status == "success");
}

TEST_CASE("auditEventFrom leaves trace ids empty when tracing is inactive", "[audit][request_context]") {
    flapi::RequestContext rc;
    flapi::RequestContext::mintRequestId(rc.request_id);
    rc.status_code = 401;

    const flapi::AuditEvent ev = flapi::auditEventFrom(rc);

    REQUIRE(ev.trace_id.empty());
    REQUIRE(ev.span_id.empty());
    REQUIRE_FALSE(ev.request_id.empty());   // request id works with tracing off
    REQUIRE(ev.status == "denied");
}

TEST_CASE("auditEventFrom maps status codes to audit status", "[audit][request_context]") {
    auto statusFor = [](int code) {
        flapi::RequestContext rc;
        flapi::RequestContext::mintRequestId(rc.request_id);
        rc.status_code = code;
        return flapi::auditEventFrom(rc).status;
    };
    REQUIRE(statusFor(200) == "success");
    REQUIRE(statusFor(201) == "success");
    REQUIRE(statusFor(401) == "denied");
    REQUIRE(statusFor(403) == "denied");
    REQUIRE(statusFor(429) == "rate_limited");
    REQUIRE(statusFor(404) == "error:404");
    REQUIRE(statusFor(500) == "error:500");
}

TEST_CASE("a serialized audit line carries trace ids only when present", "[audit][request_context]") {
    std::ostringstream out;
    flapi::AuditConfig cfg;
    cfg.enabled = true;
    cfg.sink = "null";
    flapi::AuditLogger logger(cfg);

    flapi::AuditEvent with;
    with.request_id = "req-0123456789abcdef";
    with.trace_id = "4bf92f3577b34da6a3ce929d0e0e4736";
    with.span_id = "00f067aa0ba902b7";
    const std::string line_with = logger.serialiseEventForTest(with);
    REQUIRE(line_with.find("\"trace_id\":\"4bf92f3577b34da6a3ce929d0e0e4736\"") != std::string::npos);

    flapi::AuditEvent without;
    without.request_id = "req-0123456789abcdef";
    const std::string line_without = logger.serialiseEventForTest(without);
    REQUIRE(line_without.find("trace_id") == std::string::npos);
}

TEST_CASE("AuditLogger: credential-shaped params are masked even when the "
          "operator never listed them",
          "[audit][security]") {
    // docs/OBSERVABILITY.md §4 promises credential-shaped keys are redacted
    // "regardless of your configuration". The audit log is written even with
    // tracing compiled out, so that promise has to hold here, not only on the
    // span path. Before this test, a declared `password` request field was
    // written to audit.jsonl in cleartext.
    flapi::AuditConfig cfg;
    cfg.enabled = true;
    cfg.sink = "null";
    cfg.redact_keys = {};        // operator listed NOTHING

    flapi::AuditLogger logger(cfg);

    flapi::AuditEvent ev;
    ev.request_id = "req-0";
    ev.params = {
        {"password", "hunter2"},
        {"Token", "eyJhbGciOi"},
        {"x-api-key", "sk-live-9911"},
        {"customer_id", "42"},     // ordinary field: must survive
    };

    const std::string line = logger.serialiseEventForTest(ev);
    INFO(line);
    REQUIRE(line.find("hunter2") == std::string::npos);
    REQUIRE(line.find("eyJhbGciOi") == std::string::npos);
    REQUIRE(line.find("sk-live-9911") == std::string::npos);
    REQUIRE(line.find("42") != std::string::npos);
}

TEST_CASE("AuditLogger: the operator redact list is case-insensitive",
          "[audit][security]") {
    flapi::AuditConfig cfg;
    cfg.enabled = true;
    cfg.sink = "null";
    cfg.redact_keys = {"tax_id"};

    flapi::AuditLogger logger(cfg);

    flapi::AuditEvent ev;
    ev.request_id = "req-0";
    ev.params = {{"Tax_Id", "DE123456789"}};

    const std::string line = logger.serialiseEventForTest(ev);
    INFO(line);
    REQUIRE(line.find("DE123456789") == std::string::npos);
}
