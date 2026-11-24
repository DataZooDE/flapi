// Issue 2 - correlating flAPI's ~658 CROW_LOG_* call sites with the request that
// produced them, without touching a single one of them.
//
// Crow exposes crow::ILogHandler (crow/logging.h:33) and
// crow::logger::setHandler (:127), so one handler reading the ambient
// RequestContext correlates every existing log line and every future one. The
// alternative - editing 658 call sites - would be a huge, conflict-prone diff
// that future code would immediately start diverging from.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include <crow/json.h>

#include "flapi_log_handler.hpp"
#include "request_context.hpp"

using namespace flapi;

TEST_CASE("text format appends the request id when a context is ambient", "[log_handler]") {
    FlapiLogHandler handler(FlapiLogHandler::Format::Text);

    RequestContext rc;
    RequestContext::mintRequestId(rc.request_id);
    RequestContextScope scope(&rc);

    const std::string line = handler.formatForTest("something happened", crow::LogLevel::Info);
    REQUIRE(line.find("something happened") != std::string::npos);
    REQUIRE(line.find(std::string(rc.requestIdView())) != std::string::npos);
}

TEST_CASE("text format is unchanged when no context is ambient", "[log_handler]") {
    FlapiLogHandler handler(FlapiLogHandler::Format::Text);
    REQUIRE(RequestContextScope::current() == nullptr);

    const std::string line = handler.formatForTest("startup message", crow::LogLevel::Info);
    REQUIRE(line.find("startup message") != std::string::npos);
    REQUIRE(line.find("request_id") == std::string::npos);
}

TEST_CASE("text format includes trace ids only when tracing is active", "[log_handler]") {
    FlapiLogHandler handler(FlapiLogHandler::Format::Text);

    RequestContext rc;
    RequestContext::mintRequestId(rc.request_id);
    {
        RequestContextScope scope(&rc);
        REQUIRE(handler.formatForTest("no trace", crow::LogLevel::Info).find("trace_id") == std::string::npos);
    }

    rc.setTraceId("4bf92f3577b34da6a3ce929d0e0e4736");
    rc.setSpanId("00f067aa0ba902b7");
    {
        RequestContextScope scope(&rc);
        const std::string line = handler.formatForTest("traced", crow::LogLevel::Info);
        REQUIRE(line.find("trace_id=4bf92f3577b34da6a3ce929d0e0e4736") != std::string::npos);
        REQUIRE(line.find("span_id=00f067aa0ba902b7") != std::string::npos);
    }
}

TEST_CASE("json format emits one valid object per line", "[log_handler]") {
    FlapiLogHandler handler(FlapiLogHandler::Format::Json);

    RequestContext rc;
    RequestContext::mintRequestId(rc.request_id);
    rc.setTraceId("4bf92f3577b34da6a3ce929d0e0e4736");
    RequestContextScope scope(&rc);

    const std::string line = handler.formatForTest("structured message", crow::LogLevel::Warning);
    REQUIRE(line.find('\n') == std::string::npos);   // exactly one line

    auto parsed = crow::json::load(line);
    REQUIRE(parsed);
    REQUIRE(parsed["level"].s() == "warning");
    REQUIRE(parsed["message"].s() == "structured message");
    REQUIRE(parsed["request_id"].s() == std::string(rc.requestIdView()));
    REQUIRE(parsed["trace_id"].s() == "4bf92f3577b34da6a3ce929d0e0e4736");
}

TEST_CASE("json format omits correlation keys when there is no context", "[log_handler]") {
    FlapiLogHandler handler(FlapiLogHandler::Format::Json);
    const std::string line = handler.formatForTest("boot", crow::LogLevel::Info);

    auto parsed = crow::json::load(line);
    REQUIRE(parsed);
    REQUIRE(parsed["message"].s() == "boot");
    REQUIRE_FALSE(parsed.has("request_id"));
    REQUIRE_FALSE(parsed.has("trace_id"));
}

TEST_CASE("a message containing a newline cannot break the JSON line", "[log_handler]") {
    // Log messages carry user- and error-derived text. An unescaped newline would
    // split one record into two and make the stream unparseable.
    FlapiLogHandler handler(FlapiLogHandler::Format::Json);
    const std::string line = handler.formatForTest("line one\nline two\"quoted\"", crow::LogLevel::Error);

    REQUIRE(line.find('\n') == std::string::npos);
    auto parsed = crow::json::load(line);
    REQUIRE(parsed);
    REQUIRE(parsed["message"].s() == "line one\nline two\"quoted\"");
}

TEST_CASE("concurrent emission does not interleave partial lines", "[log_handler]") {
    // The handler is installed once and called from every pooled Crow worker. If
    // it wrote in pieces, two threads would interleave into unparseable garbage.
    FlapiLogHandler handler(FlapiLogHandler::Format::Json);

    std::atomic<int> bad{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&handler, &bad, t] {
            RequestContext rc;
            RequestContext::mintRequestId(rc.request_id);
            RequestContextScope scope(&rc);
            for (int i = 0; i < 200; ++i) {
                const std::string line =
                    handler.formatForTest("thread " + std::to_string(t), crow::LogLevel::Debug);
                auto parsed = crow::json::load(line);
                if (!parsed || parsed["request_id"].s() != std::string(rc.requestIdView())) {
                    bad.fetch_add(1);
                }
            }
        });
    }
    for (auto& th : threads) { th.join(); }
    REQUIRE(bad.load() == 0);
}

// #252: the DuckDB HTTP request log attributes a line to a request that is not
// ambient on the logging thread (a DuckDB worker), or to none ("-").
namespace {
std::size_t countOf(const std::string& haystack, const std::string& needle) {
    std::size_t n = 0;
    for (auto pos = haystack.find(needle); pos != std::string::npos; pos = haystack.find(needle, pos + 1)) {
        ++n;
    }
    return n;
}
}  // namespace

TEST_CASE("a request id override is stamped exactly once, in text and JSON", "[log_handler]") {
    FlapiLogHandler text(FlapiLogHandler::Format::Text);
    FlapiLogHandler json(FlapiLogHandler::Format::Json);

    SECTION("no request ambient: the override alone") {
        FlapiLogHandler::RequestIdOverride attribute("-");
        const auto line = text.formatForTest("duckdb http GET http://x/a -> 200 1ms", crow::LogLevel::Info);
        REQUIRE(countOf(line, "request_id=") == 1);
        REQUIRE(line.find("request_id=-") != std::string::npos);
        const auto rec = crow::json::load(json.formatForTest("duckdb http GET http://x/a -> 200 1ms",
                                                             crow::LogLevel::Info));
        REQUIRE(rec);
        REQUIRE(std::string(rec["request_id"].s()) == "-");
    }
    SECTION("another request ambient: the override wins, without that request's trace") {
        RequestContext rc;
        RequestContext::mintRequestId(rc.request_id);
        rc.setTraceId("4bf92f3577b34da6a3ce929d0e0e4736");
        rc.setSpanId("00f067aa0ba902b7");
        RequestContextScope scope(&rc);
        FlapiLogHandler::RequestIdOverride attribute("req-0123456789abcdef");
        const auto line = text.formatForTest("m", crow::LogLevel::Info);
        REQUIRE(countOf(line, "request_id=") == 1);
        REQUIRE(line.find("request_id=req-0123456789abcdef") != std::string::npos);
        REQUIRE(line.find("trace_id") == std::string::npos);
    }
    SECTION("the ambient request's own id: its trace ids stay") {
        RequestContext rc;
        RequestContext::mintRequestId(rc.request_id);
        rc.setTraceId("4bf92f3577b34da6a3ce929d0e0e4736");
        rc.setSpanId("00f067aa0ba902b7");
        RequestContextScope scope(&rc);
        const std::string id(rc.requestIdView());
        FlapiLogHandler::RequestIdOverride attribute(id);
        const auto line = text.formatForTest("m", crow::LogLevel::Info);
        REQUIRE(countOf(line, "request_id=") == 1);
        REQUIRE(line.find("trace_id=4bf92f3577b34da6a3ce929d0e0e4736") != std::string::npos);
    }
    SECTION("the override ends with its scope") {
        {
            FlapiLogHandler::RequestIdOverride attribute("-");
        }
        REQUIRE(text.formatForTest("m", crow::LogLevel::Info).find("request_id") == std::string::npos);
    }
}

TEST_CASE("text format escapes CR and LF, so a message cannot forge a line", "[log_handler][security]") {
    FlapiLogHandler handler(FlapiLogHandler::Format::Text);
    const std::string line =
        handler.formatForTest("value=a\r\n(2026-01-01T00:00:00Z) [info] forged request_id=FAKE", crow::LogLevel::Info);
    INFO(line);
    REQUIRE(line.find('\n') == std::string::npos);
    REQUIRE(line.find('\r') == std::string::npos);
    REQUIRE(line.find("a\\r\\n(2026") != std::string::npos);
}
