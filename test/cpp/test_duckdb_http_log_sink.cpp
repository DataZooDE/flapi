#include <catch2/catch_all.hpp>

#include "duckdb_http_log_sink.hpp"
#include "request_context.hpp"

#include <algorithm>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "duckdb.hpp"
#include "duckdb/logging/log_type.hpp"
#include "duckdb/main/http/http_util.hpp"

using namespace flapi;

// The HTTP log message is produced by DuckDB 2.0.1's own
// HTTPLogType::ConstructLogMessage, so these tests pin the parser to what DuckDB
// really writes rather than to a hand-written imitation of it.
namespace {

std::string realMessage(duckdb::RequestType type, const std::string& url, duckdb::HTTPResponse* response,
                        bool redact) {
    duckdb::HTTPUtil util;
    duckdb::HTTPParams params(util);
    duckdb::HTTPHeaders headers;
    headers.Insert("Range", "bytes=0-1023");
    headers.Insert("Authorization", "AWS4-HMAC-SHA256 Credential=AKIATESTKEY/x, Signature=SECRETSIG");
    headers.Insert("User-Agent", "duckdb/v2.0.1(linux_amd64) cpp, with, commas = and equals");
    duckdb::BaseRequest request(type, url, headers, params);
    request.have_request_timing = true;
    request.request_system_start = duckdb::Timestamp::GetCurrentTimestamp();
    request.request_monotonic_start = duckdb::TimePoint::Tick();
    request.request_monotonic_end = request.request_monotonic_start;
    return duckdb::HTTPLogType::ConstructLogMessage(request, response, redact);
}

duckdb::HTTPResponse response(duckdb::HTTPStatusCode code) {
    duckdb::HTTPResponse r(code);
    r.headers.Insert("Content-Range", "bytes 0-1023/2048");
    r.headers.Insert("ETag", "\"x\"");
    r.reason = "Partial Content";
    return r;
}

}  // namespace

TEST_CASE("a real DuckDB HTTP log message parses into method, url, status and duration",
          "[duckdb_http_log]") {
    auto r = response(duckdb::HTTPStatusCode::PartialContent_206);
    const auto message = realMessage(duckdb::RequestType::GET_REQUEST,
                                     "http://127.0.0.1:9000/mybucket/data/obj0.csv", &r, true);
    INFO(message);
    const auto entry = ParseDuckDBHttpLogMessage(message);
    REQUIRE(entry.has_value());
    CHECK(entry->method == "GET");
    CHECK(entry->url == "http://127.0.0.1:9000/mybucket/data/obj0.csv");
    CHECK(entry->status == "206");
    CHECK(entry->duration_ms >= 0);
    CHECK(FormatDuckDBHttpLogLine(*entry).rfind("duckdb http GET http://127.0.0.1:9000/mybucket/data/obj0.csv -> 206 ",
                                                0) == 0);
}

TEST_CASE("the status is the number from DuckDB's enum name", "[duckdb_http_log]") {
    for (auto [code, expected] : std::vector<std::pair<duckdb::HTTPStatusCode, std::string>>{
             {duckdb::HTTPStatusCode::OK_200, "200"},
             {duckdb::HTTPStatusCode::NotFound_404, "404"},
             {duckdb::HTTPStatusCode::Forbidden_403, "403"},
             {duckdb::HTTPStatusCode::InternalServerError_500, "500"},
             // INVALID: the client got no response (connection refused, timeout).
             {duckdb::HTTPStatusCode::INVALID, "ERR"},
         }) {
        auto r = response(code);
        const auto entry = ParseDuckDBHttpLogMessage(
            realMessage(duckdb::RequestType::HEAD_REQUEST, "https://example.com/a", &r, true));
        REQUIRE(entry.has_value());
        CHECK(entry->method == "HEAD");
        CHECK(entry->status == expected);
    }
}

TEST_CASE("a request with no response is ERR", "[duckdb_http_log]") {
    const auto entry =
        ParseDuckDBHttpLogMessage(realMessage(duckdb::RequestType::GET_REQUEST, "http://127.0.0.1:1/x.csv", nullptr, true));
    REQUIRE(entry.has_value());
    CHECK(entry->status == "ERR");
    CHECK(FormatDuckDBHttpLogLine(*entry).find(" -> ERR ") != std::string::npos);
}

TEST_CASE("the formatted line never carries credentials, even with DuckDB's redaction off",
          "[duckdb_http_log][security]") {
    auto r = response(duckdb::HTTPStatusCode::OK_200);
    for (const bool duckdb_redacts : {true, false}) {
        INFO("redact_http_logs = " << duckdb_redacts);
        const auto message = realMessage(
            duckdb::RequestType::GET_REQUEST,
            "http://flapiuser:PWSECRET99@127.0.0.1:9000/mybucket/obj0.csv"
            "?X-Amz-Credential=AKIATESTKEY%2F20261124&X-Amz-Signature=SECRETSIG&page=2",
            &r, duckdb_redacts);
        const auto entry = ParseDuckDBHttpLogMessage(message);
        REQUIRE(entry.has_value());
        const auto line = FormatDuckDBHttpLogLine(*entry);
        INFO(line);
        for (const char* secret : {"PWSECRET99", "flapiuser", "AKIATESTKEY", "SECRETSIG", "Authorization", "Range"}) {
            CHECK(line.find(secret) == std::string::npos);
        }
        CHECK(line.find("127.0.0.1:9000/mybucket/obj0.csv") != std::string::npos);
        // Exactly one line, no request id: the log handler adds that.
        CHECK(line.find('\n') == std::string::npos);
        CHECK(line.find("request_id") == std::string::npos);
    }
}

TEST_CASE("a message that is not DuckDB's HTTP struct does not parse", "[duckdb_http_log]") {
    CHECK_FALSE(ParseDuckDBHttpLogMessage("").has_value());
    CHECK_FALSE(ParseDuckDBHttpLogMessage("GET http://x -> 200").has_value());
}

TEST_CASE("attribution guards: nested, scoped and concurrent", "[duckdb_http_log]") {
    int db_a = 0;
    int db_b = 0;
    SECTION("scoped, keyed by database and connection") {
        {
            HttpRequestAttributionGuard guard(&db_a, 7, "req-aaaaaaaaaaaaaaaa");
            CHECK(LookupHttpRequestAttribution(&db_a, 7) == std::optional<std::string>("req-aaaaaaaaaaaaaaaa"));
            CHECK_FALSE(LookupHttpRequestAttribution(&db_b, 7).has_value());   // another database's id 7
            CHECK_FALSE(LookupHttpRequestAttribution(&db_a, 8).has_value());
            {
                // Nested execution on the same connection: the outer id stays, and
                // the inner guard's end does not erase it.
                HttpRequestAttributionGuard inner(&db_a, 7, "req-bbbbbbbbbbbbbbbb");
                CHECK(LookupHttpRequestAttribution(&db_a, 7) == std::optional<std::string>("req-aaaaaaaaaaaaaaaa"));
            }
            CHECK(LookupHttpRequestAttribution(&db_a, 7) == std::optional<std::string>("req-aaaaaaaaaaaaaaaa"));
        }
        CHECK_FALSE(LookupHttpRequestAttribution(&db_a, 7).has_value());
    }
    SECTION("concurrent guards never see each other's id") {
        constexpr int kThreads = 8;
        constexpr int kIterations = 2000;
        std::atomic<int> wrong{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&, t] {
                const std::string id = "req-" + std::string(15, static_cast<char>('a' + t)) + "x";
                for (int i = 0; i < kIterations; ++i) {
                    HttpRequestAttributionGuard guard(&db_a, static_cast<std::uint64_t>(t), id);
                    const auto seen = LookupHttpRequestAttribution(&db_a, static_cast<std::uint64_t>(t));
                    if (!seen || *seen != id) {
                        ++wrong;
                    }
                }
            });
        }
        for (auto& thread : threads) {
            thread.join();
        }
        CHECK(wrong.load() == 0);
        for (int t = 0; t < kThreads; ++t) {
            CHECK_FALSE(LookupHttpRequestAttribution(&db_a, static_cast<std::uint64_t>(t)).has_value());
        }
    }
}

// Review S1/S2: what DuckDB logs with `redact_http_logs = false` (a template can
// set it at any time). The line must hold no credential from the authority or
// the query, and no byte that could end the line or start a field.
TEST_CASE("the URL in the line keeps scheme, host and path only, whatever DuckDB passed",
          "[duckdb_http_log][security]") {
    struct Case {
        const char* url;
        const char* expected;
    };
    const Case cases[] = {
        // colon-less userinfo, and an '@' in the path
        {"https://ghp_SECRET@raw.githubusercontent.com/o/r/main/@latest.csv",
         "https://raw.githubusercontent.com/o/r/main/@latest.csv"},
        // user:pw, and an '@' in the path must not swallow the host
        {"http://user:SECRETPW@host:9000/a@b", "http://host:9000/a@b"},
        {"https://h/x?jwt=SECRET1&hmac=SECRET2&session=SECRET3&sid=SECRET4&code=SECRET5&ticket=SECRET6"
         "&bearer=SECRET7&client_assertion=SECRET8&pw=SECRET9",
         "https://h/x?redacted"},
        {"https://h/x?u=x&p=SECRETP", "https://h/x?redacted"},
        {"https://h/x?SECRETVALUE", "https://h/x?redacted"},
        {"https://h/x?%73ig=SECRETS1&%74oken=SECRETS2", "https://h/x?redacted"},
        {"https://h/x?a=1;X-Amz-Signature=SECRETSIG", "https://h/x?redacted"},
        {"https://h/x#SECRETFRAG", "https://h/x"},
        {"https://a:b@h/x?q=1#f", "https://h/x?redacted"},
    };
    for (const auto& c : cases) {
        INFO(c.url);
        DuckDBHttpLogEntry entry{"GET", c.url, "200", 3};
        CHECK(FormatDuckDBHttpLogLine(entry) == std::string("duckdb http GET ") + c.expected + " -> 200 3ms");
    }
}

TEST_CASE("URL bytes that could forge a line or a field are percent-encoded", "[duckdb_http_log][security]") {
    DuckDBHttpLogEntry entry{"GET",
                             "http://h/obj0.csv\r\nFORGED [info] duckdb http GET http://x -> 200 1ms request_id=FAKE "
                             "\"q\" \xff\x7f\t",
                             "200", 1};
    const auto line = FormatDuckDBHttpLogLine(entry);
    INFO(line);
    for (const unsigned char c : line) {
        CHECK((c == ' ' || (c > 0x20 && c < 0x7f && c != '"')));
    }
    // duckdb http GET <url> -> 200 1ms: the URL is one token.
    CHECK(std::count(line.begin(), line.end(), ' ') == 6);
    CHECK(line.find("%0D%0AFORGED%20") != std::string::npos);
    CHECK(line.find("%FF%7F%09") != std::string::npos);
    CHECK(line.find("%22q%22") != std::string::npos);
}

TEST_CASE("the thread's own request wins over the connection map", "[duckdb_http_log]") {
    int db = 0;
    HttpRequestAttributionGuard mapped(&db, 3, "req-mmmmmmmmmmmmmmmm");
    SECTION("a worker thread (no request) takes the map") {
        CHECK(ResolveHttpRequestId(&db, 3) == "req-mmmmmmmmmmmmmmmm");
        CHECK(ResolveHttpRequestId(&db, 4) == "-");
        CHECK(ResolveHttpRequestId(&db, std::nullopt) == "-");
    }
    SECTION("a request thread keeps its own id, even for a connection mapped to another") {
        flapi::RequestContext rc;
        flapi::RequestContext::mintRequestId(rc.request_id);
        flapi::RequestContextScope scope(&rc);
        CHECK(ResolveHttpRequestId(&db, 3) == std::string(rc.requestIdView()));
    }
}

TEST_CASE("the switch can be reset between tests", "[duckdb_http_log]") {
    ResolveHttpRequestLog(true);
    if (!HttpRequestLogEnvSwitch().has_value()) {
        CHECK(HttpRequestLogEnabled());
    }
    ResetHttpRequestLogForTest();
    CHECK(HttpRequestLogEnabled() == HttpRequestLogEnvSwitch().value_or(false));
}

TEST_CASE("quotes, braces and commas in a URL survive DuckDB's STRUCT round trip", "[duckdb_http_log]") {
    // Review S3 expected a `}` to break the cast. In DuckDB 2.0.1 it does not:
    // ToString quotes the VARCHAR children. Pinned, with redaction off.
    auto r = response(duckdb::HTTPStatusCode::OK_200);
    for (const char* url : {"http://h/a?q=}", "http://h/a}", "http://h/a?q='", "http://h/a?q={", "http://h/a?q=\\",
                            "http://h/a?q=a, b", "http://h/a?q='}", "http://h/a?q=\"", "http://h/a?q=a': 'b"}) {
        INFO(url);
        const auto entry = ParseDuckDBHttpLogMessage(realMessage(duckdb::RequestType::GET_REQUEST, url, &r, false));
        REQUIRE(entry.has_value());
        CHECK(entry->url == url);
    }
}

TEST_CASE("a message that does not parse still yields a line, with nothing from the message",
          "[duckdb_http_log][security]") {
    CHECK(DuckDBHttpLogLineFor("{'request': garbage SECRET}") == "duckdb http ? <unparsed> -> ?");
    auto r = response(duckdb::HTTPStatusCode::OK_200);
    CHECK(DuckDBHttpLogLineFor(realMessage(duckdb::RequestType::GET_REQUEST, "http://h/a?k=SECRET", &r, false)) ==
          "duckdb http GET http://h/a?redacted -> 200 0ms");
}
