// Regression tests for #211 cluster E3: CORS preflight headers, the 401
// challenge, `auth:` blocks that name no scheme, and the auditor's wording.
#include <catch2/catch_test_macros.hpp>
#include <crow/http_request.h>
#include <crow/http_response.h>
#include <crow/json.h>
#include <jwt-cpp/jwt.h>

#include <algorithm>

#include "auth_middleware.hpp"
#include "config_manager.hpp"
#include "cors_middleware.hpp"
#include "cors_policy.hpp"
#include "security_auditor.hpp"
#include "test_utils.hpp"

namespace flapi {
namespace test {

namespace {

const char* kBase = R"(
project-name: e3
http-port: 8080
template:
  path: ./sqls
connections:
  test:
    properties:
      path: ./data.parquet
)";

const char* kGlobalBearer = R"(
auth:
  enabled: true
  type: bearer
  jwt-secret: a-long-enough-test-secret
  jwt-issuer: tests
)";

std::string endpointYaml(const std::string& path, const std::string& auth_block,
                         const std::string& method = "GET") {
    return "url-path: " + path + "\nmethod: " + method +
           "\ntemplate-source: q.sql\nconnection: [test]\n" + auth_block + "\n";
}

std::string token(const std::set<std::string>& roles) {
    return "Bearer " + jwt::create()
                           .set_issuer("tests")
                           .set_payload_claim("sub", jwt::claim(std::string("ann")))
                           .set_payload_claim("roles", jwt::claim(roles))
                           .sign(jwt::algorithm::hs256{"a-long-enough-test-secret"});
}

crow::response call(AuthMiddleware& middleware, const std::string& url, const std::string& header) {
    crow::request req;
    crow::response res;
    AuthMiddleware::context ctx;
    req.url = url;
    req.method = crow::HTTPMethod::Get;
    if (!header.empty()) {
        req.add_header("Authorization", header);
    }
    middleware.before_handle(req, res, ctx);
    return res;
}

const SecurityWarning* find(const std::vector<SecurityWarning>& warnings, const std::string& code,
                            const std::string& location) {
    for (const auto& w : warnings) {
        if (w.code == code && w.location == location) {
            return &w;
        }
    }
    return nullptr;
}

}  // namespace

TEST_CASE("CorsPolicy: preflight Allow-Headers", "[security][cors]") {
    CorsPolicy policy;
    SECTION("requested headers are echoed when none are configured") {
        REQUIRE(policy.resolveAllowHeaders({}, "authorization, content-type", true) ==
                "authorization, content-type");
    }
    SECTION("a configured list wins over the request") {
        REQUIRE(policy.resolveAllowHeaders({"X-One", "Authorization"}, "x-two", true) ==
                "X-One, Authorization");
    }
    SECTION("a configured * behaves like no list") {
        REQUIRE(policy.resolveAllowHeaders({"*"}, "authorization", false) == "authorization");
    }
    SECTION("nothing requested: * plus the credential headers by name when auth is on") {
        const auto value = policy.resolveAllowHeaders({}, "", true);
        for (const auto& h : CorsPolicy::credentialHeaders()) {
            REQUIRE(value.find(h) != std::string::npos);
        }
        REQUIRE(policy.resolveAllowHeaders({}, "", false) == "*");
    }
    SECTION("methods default and configured") {
        REQUIRE(policy.resolveAllowMethods({}) == "GET, POST, PUT, PATCH, DELETE");
        REQUIRE(policy.resolveAllowMethods({"GET", "POST"}) == "GET, POST");
    }
}

TEST_CASE("FlapiCorsMiddleware answers a preflight with every Allow-* header", "[security][cors]") {
    TempTestConfig temp(std::string(kBase) + "cors:\n  allow-origins: ['https://app.example.com']\n" +
                            kGlobalBearer,
                        "e3_cors");
    temp.writeSqlTemplate("q.sql", "SELECT 1");
    temp.writeEndpoint("p.yaml", endpointYaml("/p", ""));
    auto cm = temp.createConfigManager();
    FlapiCorsMiddleware cors;
    cors.initialize(cm);

    crow::request req;
    crow::response res;
    FlapiCorsMiddleware::context ctx;
    req.method = crow::HTTPMethod::Options;
    req.add_header("Origin", "https://app.example.com");
    req.add_header("Access-Control-Request-Method", "GET");
    cors.after_handle(req, res, ctx);

    REQUIRE(res.get_header_value("Access-Control-Allow-Origin") == "https://app.example.com");
    REQUIRE(res.get_header_value("Vary") == "Origin");
    REQUIRE(res.get_header_value("Access-Control-Allow-Methods").find("GET") != std::string::npos);
    REQUIRE(res.get_header_value("Access-Control-Allow-Headers").find("Authorization") != std::string::npos);
    REQUIRE(res.get_header_value("Access-Control-Max-Age") == "600");

    SECTION("a non-preflight response gets no preflight headers") {
        crow::request get;
        crow::response get_res;
        get.method = crow::HTTPMethod::Get;
        get.add_header("Origin", "https://app.example.com");
        cors.after_handle(get, get_res, ctx);
        REQUIRE(get_res.get_header_value("Access-Control-Allow-Origin") == "https://app.example.com");
        REQUIRE(get_res.get_header_value("Access-Control-Max-Age").empty());
    }
}

TEST_CASE("401 challenges name the endpoint's scheme and carry a JSON body", "[auth][challenge]") {
    REQUIRE(AuthMiddleware::challengeFor("basic", false) == "Basic realm=\"flAPI\"");
    REQUIRE(AuthMiddleware::challengeFor("bearer", false) == "Bearer realm=\"flAPI\"");
    REQUIRE(AuthMiddleware::challengeFor("bearer", true) == "Bearer realm=\"flAPI\", error=\"invalid_token\"");
    REQUIRE(AuthMiddleware::challengeFor("oidc", true) == "Bearer realm=\"flAPI\", error=\"invalid_token\"");

    TempTestConfig temp(std::string(kBase) + kGlobalBearer, "e3_challenge");
    temp.writeSqlTemplate("q.sql", "SELECT 1");
    temp.writeEndpoint("p.yaml", endpointYaml("/p", ""));
    auto cm = temp.createConfigManager();
    AuthMiddleware middleware;
    middleware.initialize(cm);

    auto none = call(middleware, "/p", "");
    REQUIRE(none.code == 401);
    REQUIRE(none.get_header_value("WWW-Authenticate") == "Bearer realm=\"flAPI\"");
    auto body = crow::json::load(none.body);
    REQUIRE(body);
    REQUIRE(std::string(body["error"].s()) == "Unauthorized");

    auto bad = call(middleware, "/p", "Bearer not-a-token");
    REQUIRE(bad.code == 401);
    REQUIRE(bad.get_header_value("WWW-Authenticate") == "Bearer realm=\"flAPI\", error=\"invalid_token\"");
    REQUIRE(crow::json::load(bad.body));
}

TEST_CASE("An auth block with only roles inherits the global credentials", "[auth][global]") {
    TempTestConfig temp(std::string(kBase) + kGlobalBearer, "e3_roles_only");
    temp.writeSqlTemplate("q.sql", "SELECT 1");
    temp.writeEndpoint("a.yaml", endpointYaml("/admin", "auth:\n  roles: [admin]"));
    auto cm = temp.createConfigManager();

    auto ep = cm->getEndpointForPathAndMethod("/admin", "GET");
    REQUIRE(ep);
    REQUIRE(ep->auth.enabled);
    REQUIRE(ep->auth.type == "bearer");
    REQUIRE(ep->auth.credentials_inherited);
    REQUIRE(ep->auth.roles == std::vector<std::string>{"admin"});

    AuthMiddleware middleware;
    middleware.initialize(cm);
    REQUIRE(call(middleware, "/admin", token({"admin"})).code == 200);
    REQUIRE(call(middleware, "/admin", token({"reader"})).code == 403);
    REQUIRE(call(middleware, "/admin", "").code == 401);

    SECTION("the YAML serialiser keeps the roles and does not freeze the type") {
        const auto file = cm->serializeEndpointConfigToYaml(*ep);
        REQUIRE(file.find("roles") != std::string::npos);
        REQUIRE(file.find("type") == std::string::npos);
    }
    SECTION("the config-service JSON path applies the same rule") {
        auto json = crow::json::load(
            R"({"url-path": "/json-admin", "method": "GET", "template-source": "q.sql",
                "auth": {"enabled": true, "roles": ["admin"]}})");
        REQUIRE(json);
        const auto created = cm->deserializeEndpointConfig(json);
        REQUIRE(created.auth.type == "bearer");
        REQUIRE(created.auth.credentials_inherited);
        REQUIRE(created.auth.jwt_secret == "a-long-enough-test-secret");
        REQUIRE(created.auth.roles == std::vector<std::string>{"admin"});
    }
}

TEST_CASE("An auth block that names no scheme and has nothing to inherit fails to load",
          "[auth][global]") {
    SECTION("roles only, no global block") {
        TempTestConfig temp(kBase, "e3_no_type");
        temp.writeSqlTemplate("q.sql", "SELECT 1");
        temp.writeEndpoint("a.yaml", endpointYaml("/admin", "auth:\n  roles: [admin]"));
        try {
            temp.createConfigManager();
            FAIL("expected AUTH_NO_TYPE");
        } catch (const std::exception& e) {
            REQUIRE(std::string(e.what()).find("AUTH_NO_TYPE") != std::string::npos);
        }
    }
    SECTION("users but no type") {
        TempTestConfig temp(std::string(kBase) + kGlobalBearer, "e3_users_no_type");
        temp.writeSqlTemplate("q.sql", "SELECT 1");
        temp.writeEndpoint("a.yaml", endpointYaml("/admin",
                                                  "auth:\n  users:\n    - username: u\n      password: p"));
        try {
            temp.createConfigManager();
            FAIL("expected AUTH_NO_TYPE");
        } catch (const std::exception& e) {
            REQUIRE(std::string(e.what()).find("AUTH_NO_TYPE") != std::string::npos);
        }
    }
    SECTION("enabled: false with no type still loads") {
        TempTestConfig temp(kBase, "e3_off");
        temp.writeSqlTemplate("q.sql", "SELECT 1");
        temp.writeEndpoint("a.yaml", endpointYaml("/open", "auth:\n  enabled: false"));
        REQUIRE_NOTHROW(temp.createConfigManager());
    }
}

TEST_CASE("Security auditor: method in locations, MCP empty secret, fail-closed wording",
          "[security][auditor]") {
    TempTestConfig temp(std::string(kBase) + "mcp:\n  enabled: true\n  auth:\n    enabled: true\n"
                                             "    type: bearer\n",
                        "e3_auditor");
    temp.writeSqlTemplate("q.sql", "SELECT 1");
    temp.writeEndpoint("o.yaml", endpointYaml("/orders", "auth:\n  type: bearer\n  jwt-secret: ''",
                                              "POST"));
    auto cm = temp.createConfigManager();
    const auto warnings = SecurityAuditor{}.audit(*cm);

    const auto* rest = find(warnings, "AUTH_EMPTY_JWT_SECRET", "endpoint POST /orders");
    REQUIRE(rest);
    REQUIRE(rest->message.find("fails closed: every token is refused") != std::string::npos);
    REQUIRE(rest->message.find("would verify") == std::string::npos);

    const auto* mcp = find(warnings, "AUTH_EMPTY_JWT_SECRET", "mcp.auth");
    REQUIRE(mcp);
}

}  // namespace test
}  // namespace flapi
