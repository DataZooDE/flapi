#include <catch2/catch_test_macros.hpp>
#include <crow/json.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

#include "audit_logger.hpp"

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
