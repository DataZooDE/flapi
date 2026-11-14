// Regression tests for REST endpoint authorisation (#211, cluster A):
//   - `auth.roles` is parsed and enforced (403 when no role matches);
//   - the top-level `auth:` block protects every endpoint without its own;
//   - `auth: {enabled: false}` on an endpoint opts out of the global block;
//   - `type: jwt` is an alias of `bearer`;
//   - the config-service JSON / YAML round trips keep roles and inheritance.
#include <catch2/catch_test_macros.hpp>
#include <crow/http_request.h>
#include <crow/http_response.h>
#include <crow/json.h>
#include <crow/utility.h>
#include <jwt-cpp/jwt.h>

#include "auth_middleware.hpp"
#include "config_manager.hpp"
#include "test_utils.hpp"

namespace flapi {
namespace test {

namespace {

std::string basicHeader(const std::string& user, const std::string& password) {
    const std::string raw = user + ":" + password;
    return "Basic " + crow::utility::base64encode(raw, raw.size());
}

std::string bearerToken(const std::string& secret, const std::string& issuer,
                        const std::string& subject, const std::set<std::string>& roles) {
    return jwt::create()
        .set_issuer(issuer)
        .set_payload_claim("sub", jwt::claim(subject))
        .set_payload_claim("roles", jwt::claim(roles))
        .sign(jwt::algorithm::hs256{secret});
}

struct Outcome {
    int code;
    bool authenticated;
    std::string body;
};

Outcome call(AuthMiddleware& middleware, const std::string& url, const std::string& auth_header) {
    crow::request req;
    crow::response res;
    AuthMiddleware::context ctx;
    req.url = url;
    req.method = crow::HTTPMethod::Get;
    if (!auth_header.empty()) {
        req.add_header("Authorization", auth_header);
    }
    middleware.before_handle(req, res, ctx);
    return Outcome{res.code, ctx.authenticated, res.body};
}

const char* kEndpointTemplate = R"(
url-path: %PATH%
method: GET
template-source: q.sql
connection: [test]
%AUTH%
)";

std::string endpointYaml(const std::string& path, const std::string& auth_block) {
    std::string out = kEndpointTemplate;
    out.replace(out.find("%PATH%"), 6, path);
    out.replace(out.find("%AUTH%"), 6, auth_block);
    return out;
}

const char* kBaseConfig = R"(
project-name: auth-fixes
http-port: 8080
template:
  path: ./sqls
connections:
  test:
    properties:
      path: ./data.parquet
)";

}  // namespace

TEST_CASE("REST auth.roles is parsed and enforced: 401 vs 403 vs 200", "[auth][roles]") {
    TempTestConfig temp(kBaseConfig, "auth_roles");
    temp.writeSqlTemplate("q.sql", "SELECT 1");
    temp.writeEndpoint("admin.yaml", endpointYaml("/admin", R"(auth:
  enabled: true
  type: basic
  roles: [admin]
  users:
    - username: alice
      password: alice-pw
      roles: [admin, user]
    - username: bob
      password: bob-pw
      roles: [user]
    - username: carol
      password: carol-pw)"));
    temp.writeEndpoint("open_roles.yaml", endpointYaml("/any-user", R"(auth:
  enabled: true
  type: basic
  users:
    - username: bob
      password: bob-pw
      roles: [user])"));
    auto cm = temp.createConfigManager();

    auto ep = cm->getEndpointForPathAndMethod("/admin", "GET");
    REQUIRE(ep);
    REQUIRE(ep->auth.roles == std::vector<std::string>{"admin"});

    AuthMiddleware middleware;
    middleware.initialize(cm);

    SECTION("no credentials -> 401") {
        auto out = call(middleware, "/admin", "");
        REQUIRE(out.code == 401);
    }
    SECTION("wrong password -> 401") {
        auto out = call(middleware, "/admin", basicHeader("alice", "nope"));
        REQUIRE(out.code == 401);
    }
    SECTION("valid user lacking the role -> 403 with a JSON body") {
        auto out = call(middleware, "/admin", basicHeader("bob", "bob-pw"));
        REQUIRE(out.code == 403);
        auto body = crow::json::load(out.body);
        REQUIRE(body);
        REQUIRE(std::string(body["error"].s()) == "Forbidden");
    }
    SECTION("valid user with no roles at all -> 403") {
        auto out = call(middleware, "/admin", basicHeader("carol", "carol-pw"));
        REQUIRE(out.code == 403);
    }
    SECTION("valid user holding the role -> passes (no response written)") {
        auto out = call(middleware, "/admin", basicHeader("alice", "alice-pw"));
        REQUIRE(out.authenticated);
        REQUIRE(out.code == 200);
        REQUIRE(out.body.empty());
    }
    SECTION("no roles configured -> any authenticated user passes") {
        auto out = call(middleware, "/any-user", basicHeader("bob", "bob-pw"));
        REQUIRE(out.authenticated);
        REQUIRE(out.code == 200);
    }
}

TEST_CASE("REST auth.roles applies to bearer tokens too", "[auth][roles]") {
    TempTestConfig temp(kBaseConfig, "auth_roles_jwt");
    temp.writeSqlTemplate("q.sql", "SELECT 1");
    temp.writeEndpoint("admin.yaml", endpointYaml("/admin", R"(auth:
  enabled: true
  type: jwt
  jwt-secret: a-long-enough-test-secret
  jwt-issuer: tests
  roles: [admin])"));
    auto cm = temp.createConfigManager();

    auto ep = cm->getEndpointForPathAndMethod("/admin", "GET");
    REQUIRE(ep);
    // `jwt` is accepted as an alias and normalised at load.
    REQUIRE(ep->auth.type == "bearer");

    AuthMiddleware middleware;
    middleware.initialize(cm);

    const auto admin = bearerToken("a-long-enough-test-secret", "tests", "ann", {"admin"});
    const auto user = bearerToken("a-long-enough-test-secret", "tests", "ben", {"user"});
    REQUIRE(call(middleware, "/admin", "Bearer " + admin).code == 200);
    REQUIRE(call(middleware, "/admin", "Bearer " + user).code == 403);
    REQUIRE(call(middleware, "/admin", "Bearer not-a-token").code == 401);
}

TEST_CASE("auth type jwt on an in-code endpoint is dispatched as bearer", "[auth][jwt]") {
    TempTestConfig temp(kBaseConfig, "auth_jwt_incode");
    auto cm = temp.createConfigManager();
    EndpointConfig ep;
    ep.urlPath = "/jwt";
    ep.auth.enabled = true;
    ep.auth.type = "jwt";
    ep.auth.jwt_secret = "a-long-enough-test-secret";
    ep.auth.jwt_issuer = "tests";
    cm->addEndpoint(ep);

    AuthMiddleware middleware;
    middleware.initialize(cm);
    const auto token = bearerToken("a-long-enough-test-secret", "tests", "ann", {"x"});
    auto out = call(middleware, "/jwt", "Bearer " + token);
    REQUIRE(out.authenticated);
    REQUIRE(out.code == 200);
}

TEST_CASE("Top-level auth block protects endpoints without their own", "[auth][global]") {
    const std::string config = std::string(kBaseConfig) + R"(
auth:
  enabled: true
  type: basic
  roles: [reader]
  users:
    - username: gina
      password: gina-pw
      roles: [reader]
    - username: hank
      password: hank-pw
      roles: [other]
)";
    TempTestConfig temp(config, "auth_global");
    temp.writeSqlTemplate("q.sql", "SELECT 1");
    temp.writeEndpoint("inherits.yaml", endpointYaml("/inherits", ""));
    temp.writeEndpoint("public.yaml", endpointYaml("/public", "auth:\n  enabled: false"));
    temp.writeEndpoint("own.yaml", endpointYaml("/own", R"(auth:
  enabled: true
  type: basic
  users:
    - username: ivan
      password: ivan-pw)"));
    auto cm = temp.createConfigManager();

    {
        auto inherits = cm->getEndpointForPathAndMethod("/inherits", "GET");
        REQUIRE(inherits);
        REQUIRE(inherits->auth.enabled);
        REQUIRE(inherits->auth.inherited);
        REQUIRE_FALSE(inherits->auth.declared);
        auto pub = cm->getEndpointForPathAndMethod("/public", "GET");
        REQUIRE(pub);
        REQUIRE_FALSE(pub->auth.enabled);
        REQUIRE(pub->auth.declared);
        REQUIRE_FALSE(pub->auth.inherited);
    }

    AuthMiddleware middleware;
    middleware.initialize(cm);

    SECTION("inheriting endpoint: 401 without, 403 wrong role, 200 right role") {
        REQUIRE(call(middleware, "/inherits", "").code == 401);
        REQUIRE(call(middleware, "/inherits", basicHeader("gina", "wrong")).code == 401);
        REQUIRE(call(middleware, "/inherits", basicHeader("hank", "hank-pw")).code == 403);
        REQUIRE(call(middleware, "/inherits", basicHeader("gina", "gina-pw")).code == 200);
    }
    SECTION("explicit opt-out stays public") {
        auto out = call(middleware, "/public", "");
        REQUIRE(out.code == 200);
        REQUIRE_FALSE(out.authenticated);
    }
    SECTION("an endpoint's own block replaces the global one entirely") {
        REQUIRE(call(middleware, "/own", basicHeader("ivan", "ivan-pw")).code == 200);
        REQUIRE(call(middleware, "/own", basicHeader("gina", "gina-pw")).code == 401);
    }
}

TEST_CASE("Top-level auth with enabled: false protects nothing", "[auth][global]") {
    const std::string config = std::string(kBaseConfig) + R"(
auth:
  enabled: false
  type: basic
  users:
    - username: gina
      password: gina-pw
)";
    TempTestConfig temp(config, "auth_global_off");
    temp.writeSqlTemplate("q.sql", "SELECT 1");
    temp.writeEndpoint("e.yaml", endpointYaml("/e", ""));
    auto cm = temp.createConfigManager();
    auto ep = cm->getEndpointForPathAndMethod("/e", "GET");
    REQUIRE(ep);
    REQUIRE_FALSE(ep->auth.enabled);
    REQUIRE_FALSE(ep->auth.inherited);
}

// Regression (#211, security): an `auth:` block that configures authentication
// but has no `enabled` key defaulted to enabled: false - the endpoint was OPEN,
// and under a global block it counted as an opt-out. Only `enabled: false` opts out.
TEST_CASE("An auth block without enabled is on when it configures authentication", "[auth][global]") {
    const std::string users = R"(
  users:
    - username: ivan
      password: ivan-pw
      roles: [reader])";

    SECTION("per endpoint, no global block") {
        TempTestConfig temp(kBaseConfig, "auth_implicit_endpoint");
        temp.writeSqlTemplate("q.sql", "SELECT 1");
        temp.writeEndpoint("typed.yaml", endpointYaml("/typed", "auth:\n  type: basic" + users));
        // `roles:` (with users) and no type: ON - and therefore a load error
        // since #211 E3 (AUTH_NO_TYPE), covered in auth_cors_ergonomics_test.
        // With a type it is on and authenticates.
        temp.writeEndpoint("roles.yaml", endpointYaml("/roles", "auth:\n  type: basic\n  roles: [reader]" + users));
        temp.writeEndpoint("off.yaml", endpointYaml("/off", "auth:\n  enabled: false\n  type: basic" + users));
        temp.writeEndpoint("empty.yaml", endpointYaml("/empty", "auth: {}"));
        auto cm = temp.createConfigManager();
        REQUIRE(cm->getEndpointForPathAndMethod("/typed", "GET")->auth.enabled);
        REQUIRE(cm->getEndpointForPathAndMethod("/roles", "GET")->auth.enabled);
        REQUIRE_FALSE(cm->getEndpointForPathAndMethod("/off", "GET")->auth.enabled);
        REQUIRE_FALSE(cm->getEndpointForPathAndMethod("/empty", "GET")->auth.enabled);

        AuthMiddleware middleware;
        middleware.initialize(cm);
        REQUIRE(call(middleware, "/typed", "").code == 401);
        REQUIRE(call(middleware, "/typed", basicHeader("ivan", "ivan-pw")).code == 200);
        REQUIRE(call(middleware, "/off", "").code == 200);
    }
    SECTION("under a global block it is not an opt-out") {
        const std::string config = std::string(kBaseConfig) + R"(
auth:
  enabled: true
  type: basic
  users:
    - username: gina
      password: gina-pw
)";
        TempTestConfig temp(config, "auth_implicit_global");
        temp.writeSqlTemplate("q.sql", "SELECT 1");
        temp.writeEndpoint("own.yaml", endpointYaml("/own", "auth:\n  type: basic" + users));
        auto cm = temp.createConfigManager();
        auto own = cm->getEndpointForPathAndMethod("/own", "GET");
        REQUIRE(own->auth.enabled);
        REQUIRE(own->auth.declared);
        AuthMiddleware middleware;
        middleware.initialize(cm);
        REQUIRE(call(middleware, "/own", "").code == 401);
    }
    SECTION("a global block without enabled") {
        const std::string config = std::string(kBaseConfig) + R"(
auth:
  type: basic
  users:
    - username: gina
      password: gina-pw
)";
        TempTestConfig temp(config, "auth_implicit_global_block");
        temp.writeSqlTemplate("q.sql", "SELECT 1");
        temp.writeEndpoint("e.yaml", endpointYaml("/e", ""));
        auto cm = temp.createConfigManager();   // used to throw: `enabled` was required
        auto ep = cm->getEndpointForPathAndMethod("/e", "GET");
        REQUIRE(ep->auth.enabled);
        REQUIRE(ep->auth.inherited);
        AuthMiddleware middleware;
        middleware.initialize(cm);
        REQUIRE(call(middleware, "/e", "").code == 401);
        REQUIRE(call(middleware, "/e", basicHeader("gina", "gina-pw")).code == 200);
    }
}

TEST_CASE("Config-service JSON round trip keeps roles and inheritance", "[auth][global][config_service]") {
    const std::string config = std::string(kBaseConfig) + R"(
auth:
  enabled: true
  type: basic
  users:
    - username: gina
      password: gina-pw
)";
    TempTestConfig temp(config, "auth_json_roundtrip");
    temp.writeSqlTemplate("q.sql", "SELECT 1");
    temp.writeEndpoint("inherits.yaml", endpointYaml("/inherits", ""));
    temp.writeEndpoint("roles.yaml", endpointYaml("/roles", R"(auth:
  enabled: true
  type: basic
  roles: [admin, ops]
  users:
    - username: ivan
      password: ivan-pw)"));
    auto cm = temp.createConfigManager();

    SECTION("roles survive serialise -> deserialise") {
        auto ep = cm->getEndpointForPathAndMethod("/roles", "GET");
        REQUIRE(ep);
        auto json = crow::json::load(cm->serializeEndpointConfig(*ep, EndpointJsonStyle::HyphenCase).dump());
        REQUIRE(json);
        REQUIRE(json["auth"]["roles"].size() == 2);
        auto back = cm->deserializeEndpointConfig(json, /*merge_existing=*/true);
        REQUIRE(back.auth.roles == std::vector<std::string>{"admin", "ops"});
        REQUIRE(back.auth.enabled);
        REQUIRE(back.auth.declared);
        // users are never serialised; a PUT keeps them from the existing endpoint
        REQUIRE(back.auth.users.size() == 1);
    }
    SECTION("an inherited endpoint stays inherited through GET -> PUT") {
        auto ep = cm->getEndpointForPathAndMethod("/inherits", "GET");
        REQUIRE(ep);
        auto json = crow::json::load(cm->serializeEndpointConfig(*ep, EndpointJsonStyle::HyphenCase).dump());
        REQUIRE(json);
        REQUIRE(json["auth"]["inherited"].b());
        auto back = cm->deserializeEndpointConfig(json, /*merge_existing=*/true);
        REQUIRE(back.auth.inherited);
        REQUIRE(back.auth.enabled);
        REQUIRE(back.auth.users.size() == 1);
        // ...and is not written into the endpoint file
        const auto yaml = cm->serializeEndpointConfigToYaml(back);
        REQUIRE(yaml.find("auth:") == std::string::npos);
    }
    SECTION("a new endpoint without auth inherits the global block") {
        auto json = crow::json::load(R"({"url-path": "/created", "method": "GET", "template-source": "q.sql"})");
        auto created = cm->deserializeEndpointConfig(json, /*merge_existing=*/false);
        REQUIRE(created.auth.enabled);
        REQUIRE(created.auth.inherited);
    }
    SECTION("an explicit opt-out via JSON is kept and persisted") {
        auto json = crow::json::load(
            R"({"url-path": "/created", "method": "GET", "template-source": "q.sql", "auth": {"enabled": false}})");
        auto created = cm->deserializeEndpointConfig(json, /*merge_existing=*/false);
        REQUIRE_FALSE(created.auth.enabled);
        REQUIRE(created.auth.declared);
        const auto yaml = cm->serializeEndpointConfigToYaml(created);
        REQUIRE(yaml.find("auth:") != std::string::npos);
        REQUIRE(yaml.find("enabled: false") != std::string::npos);
    }
}

TEST_CASE("AuthMiddleware::hasRequiredRole", "[auth][roles]") {
    REQUIRE(AuthMiddleware::hasRequiredRole({}, {}));
    REQUIRE(AuthMiddleware::hasRequiredRole({}, {"x"}));
    REQUIRE_FALSE(AuthMiddleware::hasRequiredRole({"admin"}, {}));
    REQUIRE_FALSE(AuthMiddleware::hasRequiredRole({"admin"}, {"Admin"}));
    REQUIRE(AuthMiddleware::hasRequiredRole({"admin", "ops"}, {"user", "ops"}));
}

}  // namespace test
}  // namespace flapi
