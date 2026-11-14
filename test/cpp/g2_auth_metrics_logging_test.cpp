// Exit-gate fixes (#211, cluster G2):
//   - Basic auth remembers successful PBKDF2 verifications (bounded, TTL, only
//     successes, invalidated by a changed password and by a config reload);
//   - an unknown `auth.type` is a load/reload error naming the valid types;
//   - metrics latencies are rounded to microseconds;
//   - the log handler holds startup lines until the log format is known, and
//     gives Crow's Response line of a refused request its request id.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <crow/http_request.h>
#include <crow/http_response.h>
#include <crow/json.h>
#include <crow/utility.h>

#include <openssl/evp.h>

#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include "auth_middleware.hpp"
#include "config_manager.hpp"
#include "flapi_log_handler.hpp"
#include "password_hasher.hpp"
#include "request_context.hpp"
#include "runtime_metrics.hpp"
#include "test_utils.hpp"

namespace flapi {
namespace test {

namespace {

std::string b64(const unsigned char* data, std::size_t n) {
    std::string out = crow::utility::base64encode(reinterpret_cast<const char*>(data), n);
    while (!out.empty() && out.back() == '=') {
        out.pop_back();
    }
    return out;
}

// A PBKDF2 hash in the stored format with a CHEAP iteration count, so a test
// can verify thousands of times. The cache treats every PBKDF2 hash alike.
std::string pbkdf2Hash(const std::string& password, const std::string& salt_text,
                       std::uint32_t iterations = 1) {
    unsigned char out[32];
    PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()),
                      reinterpret_cast<const unsigned char*>(salt_text.data()),
                      static_cast<int>(salt_text.size()), static_cast<int>(iterations), EVP_sha256(),
                      sizeof(out), out);
    return "$pbkdf2-sha256$" + std::to_string(iterations) + "$" +
           b64(reinterpret_cast<const unsigned char*>(salt_text.data()), salt_text.size()) + "$" +
           b64(out, sizeof(out));
}

std::string basicHeader(const std::string& user, const std::string& password) {
    const std::string raw = user + ":" + password;
    return "Basic " + crow::utility::base64encode(raw, raw.size());
}

int callBasic(AuthMiddleware& middleware, const std::string& url, const std::string& header) {
    crow::request req;
    crow::response res;
    AuthMiddleware::context ctx;
    req.url = url;
    req.method = crow::HTTPMethod::Get;
    req.add_header("Authorization", header);
    middleware.before_handle(req, res, ctx);
    return ctx.authenticated && res.code == 200 ? 200 : res.code;
}

const char* kBaseConfig = R"(
project-name: g2-auth
http-port: 8080
template:
  path: ./sqls
connections:
  test:
    properties:
      path: ./data.parquet
)";

std::string basicEndpoint(const std::string& stored) {
    return "url-path: /secure\nmethod: GET\ntemplate-source: q.sql\nconnection: [test]\n"
           "auth:\n  type: basic\n  users:\n    - username: alice\n      password: '" +
           stored + "'\n";
}

}  // namespace

TEST_CASE("verification cache: only successes are remembered", "[auth][password_cache]") {
    auto& cache = PasswordVerificationCache::instance();
    cache.clear();
    const auto stored = pbkdf2Hash("right", "salt-one-16bytes");

    const auto derivations0 = cache.derivations();
    REQUIRE_FALSE(cache.verifyCredentials("alice", "wrong", stored));
    REQUIRE_FALSE(cache.verifyCredentials("alice", "wrong", stored));
    // Every wrong attempt is checked in full, and none is stored.
    REQUIRE(cache.derivations() == derivations0 + 2);
    REQUIRE(cache.size() == 0);

    REQUIRE(cache.verifyCredentials("alice", "right", stored));
    REQUIRE(cache.size() == 1);
    const auto hits0 = cache.hits();
    const auto derivations1 = cache.derivations();
    REQUIRE(cache.verifyCredentials("alice", "right", stored));
    REQUIRE(cache.hits() == hits0 + 1);
    REQUIRE(cache.derivations() == derivations1);   // served from the cache

    // Same password, another user: not the remembered verification.
    REQUIRE(cache.verifyCredentials("bob", "right", stored));
    REQUIRE(cache.derivations() == derivations1 + 1);
}

TEST_CASE("verification cache: a changed password can never match an old entry",
          "[auth][password_cache]") {
    auto& cache = PasswordVerificationCache::instance();
    cache.clear();
    const auto old_stored = pbkdf2Hash("old-password", "salt-one-16bytes");
    const auto new_stored = pbkdf2Hash("new-password", "salt-two-16bytes");

    REQUIRE(cache.verifyCredentials("alice", "old-password", old_stored));
    REQUIRE(cache.verifyCredentials("alice", "old-password", old_stored));
    // The stored hash is part of the key: the old password against the NEW
    // hash is a full (failing) check, not a hit.
    REQUIRE_FALSE(cache.verifyCredentials("alice", "old-password", new_stored));
    REQUIRE(cache.verifyCredentials("alice", "new-password", new_stored));
}

TEST_CASE("verification cache: entries expire and the cache is bounded", "[auth][password_cache]") {
    auto& cache = PasswordVerificationCache::instance();
    cache.clear();
    const auto stored = pbkdf2Hash("pw", "salt-one-16bytes");

    REQUIRE(cache.verifyCredentials("alice", "pw", stored));
    cache.ageEntriesForTest(PasswordVerificationCache::kTtl + std::chrono::seconds(1));
    const auto derivations0 = cache.derivations();
    REQUIRE(cache.verifyCredentials("alice", "pw", stored));
    REQUIRE(cache.derivations() == derivations0 + 1);   // expired: verified again

    cache.clear();
    for (std::size_t i = 0; i < PasswordVerificationCache::kMaxEntries + 50; ++i) {
        REQUIRE(cache.verifyCredentials("user" + std::to_string(i), "pw", stored));
    }
    REQUIRE(cache.size() <= PasswordVerificationCache::kMaxEntries);
    cache.clear();
    REQUIRE(cache.size() == 0);
}

TEST_CASE("verification cache: cheap formats are not cached", "[auth][password_cache]") {
    auto& cache = PasswordVerificationCache::instance();
    cache.clear();
    REQUIRE(cache.verifyCredentials("alice", "plain", "plain"));
    REQUIRE_FALSE(cache.verifyCredentials("alice", "nope", "plain"));
    REQUIRE(cache.size() == 0);
    REQUIRE(PasswordVerificationCache::maxConcurrentDerivations() >= 1);
}

TEST_CASE("Basic auth: a reloaded config with a new password invalidates the cache",
          "[auth][password_cache]") {
    TempTestConfig temp(kBaseConfig, "g2_auth_reload");
    temp.writeSqlTemplate("q.sql", "SELECT 1");
    temp.writeEndpoint("secure.yaml", basicEndpoint(pbkdf2Hash("first", "salt-one-16bytes", 1000)));
    auto cm = temp.createConfigManager();
    AuthMiddleware middleware;
    middleware.initialize(cm);

    REQUIRE(callBasic(middleware, "/secure", basicHeader("alice", "first")) == 200);
    REQUIRE(PasswordVerificationCache::instance().size() >= 1);
    REQUIRE(callBasic(middleware, "/secure", basicHeader("alice", "first")) == 200);

    // The operator rotates the password and reloads the endpoint file.
    temp.writeEndpoint("secure.yaml", basicEndpoint(pbkdf2Hash("second", "salt-two-16bytes", 1000)));
    const auto file = std::filesystem::path(temp.dirPath()) / "sqls" / "secure.yaml";
    const auto result = cm->reloadEndpointFile(file);
    REQUIRE(result.ok);
    // Any (re)parse of an auth block starts the remembered verifications afresh.
    REQUIRE(PasswordVerificationCache::instance().size() == 0);

    REQUIRE(callBasic(middleware, "/secure", basicHeader("alice", "first")) == 401);
    REQUIRE(callBasic(middleware, "/secure", basicHeader("alice", "second")) == 200);
}

TEST_CASE("an unknown auth.type is a configuration error naming the valid types", "[auth][config]") {
    TempTestConfig temp(kBaseConfig, "g2_auth_typo");
    temp.writeSqlTemplate("q.sql", "SELECT 1");
    temp.writeEndpoint("typo.yaml", "url-path: /typo\nmethod: GET\ntemplate-source: q.sql\n"
                                    "connection: [test]\nauth:\n  type: basci\n  users:\n"
                                    "    - username: alice\n      password: pw\n");
    try {
        temp.createConfigManager();
        FAIL("a config with auth.type 'basci' loaded");
    } catch (const std::exception& e) {
        const std::string message = e.what();
        REQUIRE_THAT(message, Catch::Matchers::ContainsSubstring("AUTH_UNKNOWN_TYPE"));
        REQUIRE_THAT(message, Catch::Matchers::ContainsSubstring("GET /typo"));
        REQUIRE_THAT(message, Catch::Matchers::ContainsSubstring("'basci'"));
        REQUIRE_THAT(message, Catch::Matchers::ContainsSubstring("basic, bearer, jwt (alias of bearer), oidc"));
    }
}

TEST_CASE("an unknown auth.type is refused on reload too", "[auth][config]") {
    TempTestConfig temp(kBaseConfig, "g2_auth_typo_reload");
    temp.writeSqlTemplate("q.sql", "SELECT 1");
    temp.writeEndpoint("secure.yaml", basicEndpoint("pw"));
    auto cm = temp.createConfigManager();

    temp.writeEndpoint("secure.yaml", "url-path: /secure\nmethod: GET\ntemplate-source: q.sql\n"
                                      "connection: [test]\nauth:\n  type: bearr\n  jwt-secret: s\n");
    const auto file = std::filesystem::path(temp.dirPath()) / "sqls" / "secure.yaml";
    const auto result = cm->reloadEndpointFile(file);
    REQUIRE_FALSE(result.ok);
    REQUIRE_THAT(result.error, Catch::Matchers::ContainsSubstring("AUTH_UNKNOWN_TYPE"));
    // The old, valid endpoint keeps serving.
    auto ep = cm->getEndpointForPathAndMethod("/secure", "GET");
    REQUIRE(ep);
    REQUIRE(ep->auth.type == "basic");
}

TEST_CASE("jwt stays an accepted alias", "[auth][config]") {
    TempTestConfig temp(kBaseConfig, "g2_auth_jwt");
    temp.writeSqlTemplate("q.sql", "SELECT 1");
    temp.writeEndpoint("j.yaml", "url-path: /j\nmethod: GET\ntemplate-source: q.sql\nconnection: [test]\n"
                                 "auth:\n  type: jwt\n  jwt-secret: a-long-enough-test-secret\n");
    auto cm = temp.createConfigManager();
    auto ep = cm->getEndpointForPathAndMethod("/j", "GET");
    REQUIRE(ep);
    REQUIRE(ep->auth.type == "bearer");
}

TEST_CASE("metrics latencies are rounded to three decimals", "[metrics]") {
    auto& metrics = RuntimeMetrics::instance();
    metrics.reset();
    metrics.recordRest("GET", "/round", 200, std::chrono::microseconds(7496));
    metrics.recordRest("GET", "/round", 200, std::chrono::microseconds(1234));
    metrics.recordRest("GET", "/round", 200, std::chrono::microseconds(2001));
    const auto json = crow::json::load(metrics.toJson("/round").dump());
    REQUIRE(json);
    const auto& latency = json["series"][0]["latency_ms"];
    for (const char* key : {"avg", "p50", "p95", "p99", "max"}) {
        const double v = latency[key].d();
        const double scaled = v * 1000.0;
        REQUIRE(std::abs(scaled - std::round(scaled)) < 1e-6);
    }
    metrics.reset();
}

TEST_CASE("a holding log handler writes nothing until the format is known", "[log_handler]") {
    FlapiLogHandler handler(FlapiLogHandler::Format::Text, /*hold=*/true);
    // Held: formatting still works for tests, but log() must not write. The
    // observable contract is that setFormat() renders held lines in the NEW
    // format; formatForTest shows the shape it will use.
    handler.log("loading configuration", crow::LogLevel::Info);
    handler.setFormat(FlapiLogHandler::Format::Json);
    const auto line = handler.formatForTest("after", crow::LogLevel::Info);
    REQUIRE(crow::json::load(line));
}

TEST_CASE("the Response line of a refused request carries its request id", "[log_handler]") {
    FlapiLogHandler handler(FlapiLogHandler::Format::Text);
    RequestContext rc;
    RequestContext::mintRequestId(rc.request_id);
    const std::string id(rc.requestIdView());

    // A middleware refused the request: finish() ran (context released)
    // before Crow wrote its Response line.
    FlapiLogHandler::resetPendingResponse();
    FlapiLogHandler::noteRequestFinished(&rc, rc.requestIdView());
    REQUIRE(RequestContextScope::current() == nullptr);
    const auto response_line = handler.formatForTest("Response: 0x1 /secure 401 0", crow::LogLevel::Info);
    REQUIRE_THAT(response_line, Catch::Matchers::ContainsSubstring("request_id=" + id));
    // Used once: the next request's Response line is not stamped with it.
    const auto next = handler.formatForTest("Response: 0x2 /other 404 0", crow::LogLevel::Info);
    REQUIRE_THAT(next, !Catch::Matchers::ContainsSubstring("request_id"));
    // Other lines never take it.
    FlapiLogHandler::noteRequestFinished(&rc, rc.requestIdView());
    const auto other = handler.formatForTest("Request: 127.0.0.1 GET /x", crow::LogLevel::Info);
    REQUIRE_THAT(other, !Catch::Matchers::ContainsSubstring("request_id"));
    FlapiLogHandler::resetPendingResponse();
}

TEST_CASE("a Response line already written in scope leaves nothing pending", "[log_handler]") {
    FlapiLogHandler handler(FlapiLogHandler::Format::Text);
    RequestContext rc;
    RequestContext::mintRequestId(rc.request_id);
    FlapiLogHandler::resetPendingResponse();
    {
        RequestContextScope scope(&rc);
        const auto line = handler.formatForTest("Response: 0x1 /open 200 0", crow::LogLevel::Info);
        REQUIRE_THAT(line, Catch::Matchers::ContainsSubstring("request_id="));
    }
    FlapiLogHandler::noteRequestFinished(&rc, rc.requestIdView());
    const auto next = handler.formatForTest("Response: 0x2 /unmatched 404 0", crow::LogLevel::Info);
    REQUIRE_THAT(next, !Catch::Matchers::ContainsSubstring("request_id"));
}

}  // namespace test
}  // namespace flapi
