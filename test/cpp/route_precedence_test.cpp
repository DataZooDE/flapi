// Routing semantics that authorization depends on (#211, exit-gate cluster H).
//
// Rate limiting, auth and the request handler each resolve the endpoint for a
// request through ConfigManager::findEndpoint. These tests pin down what that
// resolution is - and what the loader refuses - so an open endpoint can never
// answer the URL of a protected one by accident of spelling or load order.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <stdexcept>

#include "config_manager.hpp"
#include "route_translator.hpp"
#include "test_utils.hpp"

using flapi::ConfigManager;
using flapi::EndpointConfig;
using flapi::RouteTranslator;

namespace {

const char* kConfig = R"(
project-name: precedence
template:
  path: ./sqls
connections:
  mem:
    properties:
      database: ':memory:'
)";

EndpointConfig rest(const std::string& path, const std::string& method = "GET", bool auth = false) {
    EndpointConfig ep;
    ep.urlPath = path;
    ep.method = method;
    ep.auth.enabled = auth;
    ep.auth.type = auth ? "basic" : "";
    return ep;
}

std::string endpointYaml(const std::string& path, const std::string& method, bool protect,
                         const std::string& extra = "") {
    std::string yaml = "url-path: " + path + "\nmethod: " + method + "\ntemplate-source: a.sql\nconnection: [mem]\n";
    if (protect) {
        yaml += "auth:\n  enabled: true\n  type: basic\n  roles: [admin]\n  users:\n"
                "    - username: u\n      password: p\n      roles: [admin]\n";
    } else {
        yaml += "auth:\n  enabled: false\n";
    }
    return yaml + extra;
}

std::string loadError(flapi::test::TempTestConfig& temp) {
    try {
        temp.createConfigManager();
    } catch (const std::exception& e) {
        return e.what();
    }
    return "";
}

}  // namespace

TEST_CASE("HTTP methods are stored canonically and limited to five", "[routing][method]") {
    CHECK(ConfigManager::CanonicalHttpMethod("get") == "GET");
    CHECK(ConfigManager::CanonicalHttpMethod(" Post\t") == "POST");
    CHECK(ConfigManager::CanonicalHttpMethod("") == "GET");
    CHECK(ConfigManager::CanonicalHttpMethod("   ") == "GET");
    for (const char* ok : {"GET", "get", "Get", "", "post", "PUT", "patch", "Delete"}) {
        CHECK(ConfigManager::HttpMethodProblem(ok, "/x").empty());
    }
    for (const char* bad : {"FETCH", "HEAD", "OPTIONS", "G ET", "*"}) {
        const auto problem = ConfigManager::HttpMethodProblem(bad, "/x");
        CHECK(problem.find("/x") != std::string::npos);
        CHECK(problem.find("GET, POST, PUT, PATCH, DELETE") != std::string::npos);
    }
}

TEST_CASE("A lower-case method loads as the canonical one", "[routing][method]") {
    flapi::test::TempTestConfig temp(kConfig, "method_canonical");
    temp.writeSqlTemplate("a.sql", "SELECT 1 AS n\n");
    temp.writeEndpoint("a.yaml", endpointYaml("/a", "get", false));
    temp.writeEndpoint("b.yaml", endpointYaml("/b", "", false));
    auto mgr = temp.createConfigManager();
    const auto a = mgr->getEndpointForPathAndMethod("/a", "GET");
    REQUIRE(a);
    CHECK(a->method == "GET");
    const auto b = mgr->getEndpointForPathAndMethod("/b", "get");
    REQUIRE(b);
    CHECK(b->method == "GET");
}

TEST_CASE("An unsupported method is a load error naming the endpoint", "[routing][method]") {
    flapi::test::TempTestConfig temp(kConfig, "method_unsupported");
    temp.writeSqlTemplate("a.sql", "SELECT 1 AS n\n");
    temp.writeEndpoint("a.yaml", endpointYaml("/a", "fetch", false));
    const auto error = loadError(temp);
    CHECK(error.find("method 'fetch' is not supported") != std::string::npos);
    CHECK(error.find("/a") != std::string::npos);
}

TEST_CASE("method: get beside a protected GET on the same route is a duplicate", "[routing][method][security]") {
    // The exit-gate bypass: compared case-sensitively, `get` loaded next to the
    // protected `GET` and routing (case-insensitive) could pick the open copy.
    for (const char* spelling : {"get", "Get", "\"\"", " GET "}) {
        flapi::test::TempTestConfig temp(kConfig, "method_duplicate");
        temp.writeSqlTemplate("a.sql", "SELECT 1 AS n\n");
        temp.writeEndpoint("a.yaml", endpointYaml("/customers/:id", "GET", true));
        temp.writeEndpoint("b.yaml", endpointYaml("/customers/:cid", spelling, false));
        const auto error = loadError(temp);
        INFO(spelling);
        CHECK(error.find("Duplicate endpoint GET /customers/:cid") != std::string::npos);
    }
}

TEST_CASE("url-paths are validated", "[routing][url-path]") {
    for (const char* ok : {"/", "/customers", "/customers/", "/customers/:id", "/a/:x/b", "/v1.2/data_set-x~y",
                           "/:_p1/x"}) {
        INFO(ok);
        CHECK(RouteTranslator::urlPathProblem(ok).empty());
    }
    for (const char* bad : {"", "customers", "/customers/{cid}", "/a/*", "/a(b)", "/a/[x]", "/a+", "/a?",
                            "/a<b>", "/a|b", "/^a$", "/a\\b", "//a", "/a//b", "/a/:", "/a/:1x", "/a/:x-y",
                            "/a/b:c", "/a b", "/a%3Aid", "/a/./b", "/a/../b", "/..", "/.", "/a/:id/b/:id",
                            "/:x/:x", "/caf\xC3\xA9"}) {
        INFO(bad);
        CHECK_FALSE(RouteTranslator::urlPathProblem(bad).empty());
    }
    CHECK(RouteTranslator::urlPathProblem("/customers/{cid}").find("`:cid`") != std::string::npos);
    // Dots inside a segment are fine; only whole `.` / `..` segments are not.
    for (const char* ok : {"/a/.well-known", "/a/..b", "/v1.2/x", "/a/:id/b/:other"}) {
        INFO(ok);
        CHECK(RouteTranslator::urlPathProblem(ok).empty());
    }
    CHECK(RouteTranslator::urlPathProblem("/a/../b").find("'..'") != std::string::npos);
    CHECK(RouteTranslator::urlPathProblem("/a/:id/b/:id").find("`:id` appears more than once") != std::string::npos);
    // A non-ASCII character is printed whole with its code point, not as one
    // byte of its UTF-8 encoding.
    const auto accent = RouteTranslator::urlPathProblem("/caf\xC3\xA9");
    CHECK(accent.find("contains '\xC3\xA9' (U+00E9)") != std::string::npos);
    CHECK(RouteTranslator::urlPathProblem("/a\xE2\x82\xAC").find("(U+20AC)") != std::string::npos);
    CHECK(RouteTranslator::urlPathProblem("/a\xFF").find("invalid UTF-8 byte 0xFF") != std::string::npos);
    CHECK(RouteTranslator::urlPathProblem("/a\xC3").find("invalid UTF-8 byte 0xC3") != std::string::npos);
}

TEST_CASE("A url-path with regex metacharacters is a load error, not a hung server", "[routing][url-path]") {
    flapi::test::TempTestConfig temp(kConfig, "urlpath_braces");
    temp.writeSqlTemplate("a.sql", "SELECT 1 AS n\n");
    temp.writeEndpoint("a.yaml", endpointYaml("/customers/{cid}", "GET", false));
    const auto error = loadError(temp);
    CHECK(error.find("/customers/{cid}") != std::string::npos);
    CHECK(error.find("`:cid`") != std::string::npos);
}

TEST_CASE("Literal route segments are matched literally and never throw", "[routing][url-path]") {
    std::vector<std::string> names;
    std::map<std::string, std::string> params;
    // `.` was copied into the regex raw and matched any character.
    CHECK(RouteTranslator::matchAndExtractParams("/data.json", "/data.json", names, params));
    CHECK_FALSE(RouteTranslator::matchAndExtractParams("/data.json", "/dataXjson", names, params));
    // Patterns that do not pass validation (built in code) still must not
    // throw out of routing: they are escaped and match only themselves.
    for (const char* pattern : {"/customers/{cid}", "/a/[x", "/a(", "/a+", "/a/*", "/a\\"}) {
        INFO(pattern);
        CHECK_NOTHROW(RouteTranslator::matchAndExtractParams(pattern, "/customers/1", names, params));
        CHECK_FALSE(RouteTranslator::matchAndExtractParams(pattern, "/customers/1", names, params));
    }
    CHECK(RouteTranslator::matchAndExtractParams("/customers/{cid}", "/customers/{cid}", names, params));
}

TEST_CASE("Route overlap and specificity", "[routing][precedence]") {
    CHECK(RouteTranslator::routesOverlap("/customers/:id", "/customers/7"));
    CHECK(RouteTranslator::routesOverlap("/a/:x/b", "/:z/:x/b"));
    CHECK(RouteTranslator::routesOverlap("/a/:x/b", "/a/:y/:z"));
    CHECK_FALSE(RouteTranslator::routesOverlap("/a/:x/b", "/a/:x/c"));
    CHECK_FALSE(RouteTranslator::routesOverlap("/a/:x", "/a/:x/b"));
    CHECK_FALSE(RouteTranslator::routesOverlap("/customers/", "/customers/:id"));   // :id needs a character
    CHECK(RouteTranslator::routesOverlap("/customers/", "/customers/"));

    // Left to right, the first literal-vs-parameter difference decides.
    CHECK(RouteTranslator::compareSpecificity("/customers/7", "/customers/:id") < 0);
    CHECK(RouteTranslator::compareSpecificity("/customers/:id", "/customers/7") > 0);
    CHECK(RouteTranslator::compareSpecificity("/a/:x/b", "/:z/:x/b") < 0);
    CHECK(RouteTranslator::compareSpecificity("/a/:x/b", "/a/:y/:z") < 0);
    CHECK(RouteTranslator::compareSpecificity("/a/:x/:y", "/:a/b/c") < 0);
    CHECK(RouteTranslator::compareSpecificity("/a/:x", "/a/:y") == 0);
}

TEST_CASE("The most specific route wins whatever the table order", "[routing][precedence][security]") {
    const std::vector<std::pair<std::vector<std::string>, std::vector<std::pair<std::string, std::string>>>> cases = {
        {{"/customers/:id", "/customers/7"}, {{"/customers/7", "/customers/7"}, {"/customers/8", "/customers/:id"}}},
        {{"/a/:x/b", "/:z/:x/b"}, {{"/a/1/b", "/a/:x/b"}, {"/c/1/b", "/:z/:x/b"}}},
        {{"/a/:x/b", "/a/:y/:z"}, {{"/a/1/b", "/a/:x/b"}, {"/a/1/c", "/a/:y/:z"}}},
        {{"/a/:x/:y", "/:a/b/c"}, {{"/a/b/c", "/a/:x/:y"}}},
    };
    for (const auto& [routes, expectations] : cases) {
        std::vector<EndpointConfig> table;
        for (const auto& route : routes) {
            table.push_back(rest(route));
        }
        std::sort(table.begin(), table.end(),
                  [](const EndpointConfig& a, const EndpointConfig& b) { return a.urlPath < b.urlPath; });
        do {
            for (const auto& [request, winner] : expectations) {
                INFO(request);
                const auto* found = ConfigManager::findEndpoint(table, request, "GET");
                REQUIRE(found != nullptr);
                CHECK(found->urlPath == winner);
                // The method-less lookup the config service uses agrees.
                const auto* any = ConfigManager::findEndpoint(table, request);
                REQUIRE(any != nullptr);
                CHECK(any->urlPath == winner);
            }
        } while (std::next_permutation(table.begin(), table.end(), [](const EndpointConfig& a,
                                                                        const EndpointConfig& b) {
            return a.urlPath < b.urlPath;
        }));
    }
}

TEST_CASE("The method is part of the match, compared canonically", "[routing][precedence]") {
    std::vector<EndpointConfig> table = {rest("/customers/7", "post"), rest("/customers/:id", "get")};
    const auto* found = ConfigManager::findEndpoint(table, "/customers/7", "GET");
    REQUIRE(found != nullptr);
    CHECK(found->urlPath == "/customers/:id");
    found = ConfigManager::findEndpoint(table, "/customers/7", "post");
    REQUIRE(found != nullptr);
    CHECK(found->urlPath == "/customers/7");
}

TEST_CASE("An overlap whose winner is looser is refused at load", "[routing][overlap][security]") {
    // Both file orders: the refusal must not depend on which loads first.
    for (const auto& [literal_file, param_file] :
         {std::pair<std::string, std::string>{"a.yaml", "b.yaml"}, {"b.yaml", "a.yaml"}}) {
        flapi::test::TempTestConfig temp(kConfig, "overlap_refused");
        temp.writeSqlTemplate("a.sql", "SELECT 1 AS n\n");
        temp.writeEndpoint(param_file, endpointYaml("/customers/:id", "GET", true));
        temp.writeEndpoint(literal_file, endpointYaml("/customers/7", "GET", false));
        const auto error = loadError(temp);
        INFO(error);
        CHECK(error.find("Overlapping routes with different auth: GET /customers/7 (open) takes precedence over "
                         "GET /customers/:id (basic, roles [admin]) for /customers/7") != std::string::npos);
        CHECK(error.find("allow-shadowing: true") != std::string::npos);
    }
}

TEST_CASE("Overlaps that are allowed: same auth, stricter winner, explicit opt-in", "[routing][overlap]") {
    SECTION("same auth") {
        flapi::test::TempTestConfig temp(kConfig, "overlap_same_auth");
        temp.writeSqlTemplate("a.sql", "SELECT 1 AS n\n");
        temp.writeEndpoint("a.yaml", endpointYaml("/customers/:id", "GET", true));
        temp.writeEndpoint("b.yaml", endpointYaml("/customers/7", "GET", true));
        CHECK(loadError(temp).empty());
    }
    SECTION("the more specific route is the protected one, with allow-shadowing") {
        flapi::test::TempTestConfig temp(kConfig, "overlap_stricter");
        temp.writeSqlTemplate("a.sql", "SELECT 1 AS n\n");
        temp.writeEndpoint("a.yaml", endpointYaml("/a/:x/b", "GET", true, "allow-shadowing: true\n"));
        temp.writeEndpoint("b.yaml", endpointYaml("/:z/:x/b", "GET", false));
        temp.writeEndpoint("c.yaml", endpointYaml("/a/:y/:z", "GET", false));
        auto mgr = temp.createConfigManager();
        const auto ep = mgr->getEndpointForPathAndMethod("/a/1/b", "GET");
        REQUIRE(ep);
        CHECK(ep->urlPath == "/a/:x/b");
        CHECK(ep->auth.enabled);
    }
    SECTION("allow-shadowing on the more specific route") {
        flapi::test::TempTestConfig temp(kConfig, "overlap_opt_in");
        temp.writeSqlTemplate("a.sql", "SELECT 1 AS n\n");
        temp.writeEndpoint("a.yaml", endpointYaml("/customers/:id", "GET", true));
        temp.writeEndpoint("b.yaml", endpointYaml("/customers/7", "GET", false, "allow-shadowing: true\n"));
        auto mgr = temp.createConfigManager();
        const auto ep = mgr->getEndpointForPathAndMethod("/customers/7", "GET");
        REQUIRE(ep);
        CHECK(ep->allow_shadowing);
        CHECK_FALSE(ep->auth.enabled);
        // It is serialised, so a persisted copy keeps the opt-in.
        CHECK(mgr->serializeEndpointConfigToYaml(*ep).find("allow-shadowing: true") != std::string::npos);
    }
    SECTION("opt-in on the LESS specific route does not count") {
        flapi::test::TempTestConfig temp(kConfig, "overlap_opt_in_wrong");
        temp.writeSqlTemplate("a.sql", "SELECT 1 AS n\n");
        temp.writeEndpoint("a.yaml", endpointYaml("/customers/:id", "GET", true, "allow-shadowing: true\n"));
        temp.writeEndpoint("b.yaml", endpointYaml("/customers/7", "GET", false));
        CHECK(loadError(temp).find("Overlapping routes with different auth") != std::string::npos);
    }
    SECTION("different roles where the winner admits more") {
        flapi::test::TempTestConfig temp(kConfig, "overlap_roles");
        temp.writeSqlTemplate("a.sql", "SELECT 1 AS n\n");
        temp.writeEndpoint("a.yaml", endpointYaml("/customers/:id", "GET", true));
        std::string wider = endpointYaml("/customers/7", "GET", true);
        wider.replace(wider.find("roles: [admin]"), std::string("roles: [admin]").size(), "roles: [admin, guest]");
        temp.writeEndpoint("b.yaml", wider);
        CHECK(loadError(temp).find("Overlapping routes with different auth") != std::string::npos);
    }
}

TEST_CASE("Reloading either file does not change which endpoint answers", "[routing][precedence][security]") {
    flapi::test::TempTestConfig temp(kConfig, "reload_precedence");
    temp.writeSqlTemplate("a.sql", "SELECT 1 AS n\n");
    // The protected literal opts in: without it this split is refused (I-1).
    const auto protected_file =
        temp.writeEndpoint("a.yaml", endpointYaml("/customers/7", "GET", true, "allow-shadowing: true\n"));
    const auto open_file = temp.writeEndpoint("b.yaml", endpointYaml("/customers/:id", "GET", false));
    auto mgr = temp.createConfigManager();

    const auto check_winner = [&]() {
        const auto ep = mgr->getEndpointForPathAndMethod("/customers/7", "GET");
        REQUIRE(ep);
        CHECK(ep->urlPath == "/customers/7");
        CHECK(ep->auth.enabled);
        const auto other = mgr->getEndpointForPathAndMethod("/customers/8", "GET");
        REQUIRE(other);
        CHECK(other->urlPath == "/customers/:id");
    };
    check_winner();
    for (int round = 0; round < 2; ++round) {
        REQUIRE(mgr->reloadEndpointFile(protected_file).ok);
        check_winner();
        REQUIRE(mgr->reloadEndpointFile(open_file).ok);
        check_winner();
    }
    // A reload keeps the table order, too.
    const auto snapshot = mgr->getEndpoints();
    REQUIRE(snapshot->size() == 2);
    CHECK((*snapshot)[0].urlPath == "/customers/7");
    CHECK((*snapshot)[1].urlPath == "/customers/:id");
}

TEST_CASE("A reload or create that would open a protected URL is refused", "[routing][overlap][security]") {
    flapi::test::TempTestConfig temp(kConfig, "reload_overlap");
    temp.writeSqlTemplate("a.sql", "SELECT 1 AS n\n");
    temp.writeEndpoint("a.yaml", endpointYaml("/customers/:id", "GET", true));
    const auto literal = temp.writeEndpoint("b.yaml", endpointYaml("/customers/7", "GET", true));
    auto mgr = temp.createConfigManager();

    // Edited to drop its auth, then reloaded: refused, the old version stays.
    temp.writeEndpoint("b.yaml", endpointYaml("/customers/7", "GET", false));
    const auto result = mgr->reloadEndpointFile(literal);
    CHECK_FALSE(result.ok);
    CHECK(result.status == 409);
    CHECK(result.error.find("Overlapping routes with different auth") != std::string::npos);
    const auto ep = mgr->getEndpointForPathAndMethod("/customers/7", "GET");
    REQUIRE(ep);
    CHECK(ep->auth.enabled);

    // In-memory candidate, as create checks it.
    EndpointConfig open_literal = rest("/customers/9");
    open_literal.config_file_path = "/elsewhere/c.yaml";
    CHECK(mgr->DuplicateEndpointProblem(open_literal).find("Overlapping routes") != std::string::npos);
    // The same endpoint as an update of itself is not compared with itself.
    const auto self = mgr->getEndpointForPathAndMethod("/customers/:id", "GET");
    REQUIRE(self);
    EndpointConfig updated = *self;
    updated.config_file_path.clear();
    CHECK(mgr->DuplicateEndpointProblem(updated).find("Duplicate endpoint") != std::string::npos);
    CHECK(mgr->DuplicateEndpointProblem(updated, &*self).empty());
}

namespace {

std::string basicYaml(const std::string& path, const std::string& user, const std::string& password,
                      const std::string& extra = "") {
    return "url-path: " + path + "\nmethod: GET\ntemplate-source: a.sql\nconnection: [mem]\n"
           "auth:\n  enabled: true\n  type: basic\n  roles: [admin]\n  users:\n"
           "    - username: " + user + "\n      password: " + password + "\n      roles: [admin]\n" + extra;
}

std::string bearerYaml(const std::string& path, const std::string& secret, const std::string& issuer) {
    return "url-path: " + path + "\nmethod: GET\ntemplate-source: a.sql\nconnection: [mem]\n"
           "auth:\n  enabled: true\n  type: bearer\n  jwt-secret: " + secret + "\n  jwt-issuer: " + issuer + "\n";
}

}  // namespace

TEST_CASE("A protected literal over an open :param route is refused (finding I-1)", "[routing][overlap][security]") {
    // `/customers/7` protected protects the string "7" only: `/customers/07`
    // and `/customers/+7` match the open `/customers/:id` and name the same row.
    for (const auto& [literal_file, param_file] :
         {std::pair<std::string, std::string>{"a.yaml", "b.yaml"}, {"b.yaml", "a.yaml"}}) {
        flapi::test::TempTestConfig temp(kConfig, "overlap_protected_literal");
        temp.writeSqlTemplate("a.sql", "SELECT 1 AS n\n");
        temp.writeEndpoint(param_file, endpointYaml("/customers/:id", "GET", false));
        temp.writeEndpoint(literal_file, endpointYaml("/customers/7", "GET", true));
        const auto error = loadError(temp);
        INFO(error);
        CHECK(error.find("Overlapping routes with different auth: GET /customers/7 (basic, roles [admin]) takes "
                         "precedence over GET /customers/:id (open) for /customers/7") != std::string::npos);
        CHECK(error.find("/customers/:id still serves every other URL its pattern matches") != std::string::npos);
        CHECK(error.find("such as /customers/07") != std::string::npos);
        CHECK(error.find("protects that exact path string, not a record") != std::string::npos);
        CHECK(error.find("allow-shadowing: true` on /customers/7") != std::string::npos);
    }
    SECTION("a narrower role set on the literal is refused too") {
        flapi::test::TempTestConfig temp(kConfig, "overlap_narrower_roles");
        temp.writeSqlTemplate("a.sql", "SELECT 1 AS n\n");
        std::string wider = endpointYaml("/customers/:id", "GET", true);
        wider.replace(wider.find("roles: [admin]"), std::string("roles: [admin]").size(), "roles: [admin, guest]");
        temp.writeEndpoint("a.yaml", wider);
        temp.writeEndpoint("b.yaml", endpointYaml("/customers/7", "GET", true));
        CHECK(loadError(temp).find("auth differs in: roles") != std::string::npos);
    }
    SECTION("allow-shadowing on the literal accepts it") {
        flapi::test::TempTestConfig temp(kConfig, "overlap_protected_literal_opt_in");
        temp.writeSqlTemplate("a.sql", "SELECT 1 AS n\n");
        temp.writeEndpoint("a.yaml", endpointYaml("/customers/:id", "GET", false));
        temp.writeEndpoint("b.yaml", endpointYaml("/customers/7", "GET", true, "allow-shadowing: true\n"));
        auto mgr = temp.createConfigManager();
        const auto ep = mgr->getEndpointForPathAndMethod("/customers/7", "GET");
        REQUIRE(ep);
        CHECK(ep->auth.enabled);
        const auto other = mgr->getEndpointForPathAndMethod("/customers/07", "GET");
        REQUIRE(other);
        CHECK(other->urlPath == "/customers/:id");
    }
}

TEST_CASE("Same scheme and roles but a different identity source is different auth (finding I-2)",
          "[routing][overlap][security]") {
    const auto refused = [](const std::string& general, const std::string& literal) {
        flapi::test::TempTestConfig temp(kConfig, "overlap_identity");
        temp.writeSqlTemplate("a.sql", "SELECT 1 AS n\n");
        temp.writeEndpoint("a.yaml", general);
        temp.writeEndpoint("b.yaml", literal);
        return loadError(temp);
    };
    SECTION("a literal with its own guest user") {
        const auto error = refused(basicYaml("/customers/:id", "admin", "s3cr3t-admin-pw"),
                                   basicYaml("/customers/7", "guest", "guest-pw"));
        INFO(error);
        CHECK(error.find("auth differs in: users") != std::string::npos);
        // Never secret material in the message.
        CHECK(error.find("s3cr3t-admin-pw") == std::string::npos);
        CHECK(error.find("guest-pw") == std::string::npos);
    }
    SECTION("the same user with a different password") {
        CHECK(refused(basicYaml("/customers/:id", "admin", "pw-one"), basicYaml("/customers/7", "admin", "pw-two"))
                  .find("auth differs in: users") != std::string::npos);
    }
    SECTION("bearer with a different jwt-secret") {
        const auto error = refused(bearerYaml("/customers/:id", "super-secret-one", "iss"),
                                   bearerYaml("/customers/7", "super-secret-two", "iss"));
        INFO(error);
        CHECK(error.find("auth differs in: jwt-secret") != std::string::npos);
        CHECK(error.find("super-secret-one") == std::string::npos);
        CHECK(error.find("super-secret-two") == std::string::npos);
    }
    SECTION("bearer with a different jwt-issuer") {
        CHECK(refused(bearerYaml("/customers/:id", "k", "issuer-a"), bearerYaml("/customers/7", "k", "issuer-b"))
                  .find("auth differs in: jwt-issuer") != std::string::npos);
    }
    SECTION("identical identity sources still load") {
        CHECK(refused(basicYaml("/customers/:id", "admin", "pw"), basicYaml("/customers/7", "admin", "pw")).empty());
        CHECK(refused(bearerYaml("/customers/:id", "k", "iss"), bearerYaml("/customers/7", "k", "iss")).empty());
    }
}

TEST_CASE("OIDC and AWS secret sources are part of the auth comparison", "[routing][overlap][security]") {
    flapi::test::TempTestConfig temp(kConfig, "overlap_oidc");
    temp.writeSqlTemplate("a.sql", "SELECT 1 AS n\n");
    auto mgr = temp.createConfigManager();
    const auto oidc = [](const std::string& path, const std::string& issuer, const std::string& audience) {
        EndpointConfig ep = rest(path);
        ep.auth.enabled = true;
        ep.auth.type = "oidc";
        flapi::OIDCConfig o;
        o.issuer_url = issuer;
        o.client_id = "client";
        o.allowed_audiences = {audience};
        ep.auth.oidc = o;
        return ep;
    };
    auto general = oidc("/customers/:id", "https://idp.example/a", "api");
    general.config_file_path = "/x/general.yaml";
    mgr->addEndpoint(general);
    auto same = oidc("/customers/7", "https://idp.example/a", "api");
    same.config_file_path = "/x/literal.yaml";
    CHECK(mgr->DuplicateEndpointProblem(same).empty());
    auto other_issuer = oidc("/customers/7", "https://idp.example/b", "api");
    other_issuer.config_file_path = "/x/literal.yaml";
    CHECK(mgr->DuplicateEndpointProblem(other_issuer).find("auth differs in: oidc") != std::string::npos);
    auto other_audience = oidc("/customers/7", "https://idp.example/a", "other");
    other_audience.config_file_path = "/x/literal.yaml";
    CHECK(mgr->DuplicateEndpointProblem(other_audience).find("auth differs in: oidc") != std::string::npos);
    // Token-validation settings change who gets in: a literal that accepts expired tokens is not "same auth".
    auto no_expiry = same;
    no_expiry.auth.oidc->verify_expiration = false;
    CHECK(mgr->DuplicateEndpointProblem(no_expiry).find("auth differs in: oidc") != std::string::npos);
    auto huge_skew = same;
    huge_skew.auth.oidc->clock_skew_seconds = 99999999;
    CHECK(mgr->DuplicateEndpointProblem(huge_skew).find("auth differs in: oidc") != std::string::npos);
    auto other_scopes = same;
    other_scopes.auth.oidc->scopes = {"openid", "admin"};
    CHECK(mgr->DuplicateEndpointProblem(other_scopes).find("auth differs in: oidc") != std::string::npos);

    auto aws = same;
    aws.auth.from_aws_secretmanager = flapi::AuthFromSecretManagerConfig{};
    aws.auth.from_aws_secretmanager->secret_name = "prod/users";
    aws.auth.from_aws_secretmanager->region = "eu-central-1";
    CHECK(mgr->DuplicateEndpointProblem(aws).find("from-aws-secretmanager") != std::string::npos);
}

TEST_CASE("RouteTranslator::percentDecodePathValue decodes once and rejects malformed input (#220)",
          "[routing][percent-decoding]") {
    std::string out;
    CHECK(RouteTranslator::percentDecodePathValue("%37", out));
    CHECK(out == "7");
    CHECK(RouteTranslator::percentDecodePathValue("John%20Doe", out));
    CHECK(out == "John Doe");
    CHECK(RouteTranslator::percentDecodePathValue("a%2Fb", out));
    CHECK(out == "a/b");
    CHECK(RouteTranslator::percentDecodePathValue("a%2fb", out));
    CHECK(out == "a/b");
    CHECK(RouteTranslator::percentDecodePathValue("a+b", out));
    CHECK(out == "a+b");                       // '+' is a plus in a path
    CHECK(RouteTranslator::percentDecodePathValue("a%252Fb", out));
    CHECK(out == "a%2Fb");                     // exactly once
    CHECK(RouteTranslator::percentDecodePathValue("caf%C3%A9", out));
    CHECK(out == "caf\xC3\xA9");
    CHECK(RouteTranslator::percentDecodePathValue("", out));
    CHECK(out.empty());

    CHECK_FALSE(RouteTranslator::percentDecodePathValue("%zz", out));
    CHECK_FALSE(RouteTranslator::percentDecodePathValue("%4", out));
    CHECK_FALSE(RouteTranslator::percentDecodePathValue("ab%", out));
    CHECK_FALSE(RouteTranslator::percentDecodePathValue("a%00b", out));
    CHECK_FALSE(RouteTranslator::percentDecodePathValue("%C3", out));     // truncated UTF-8
    CHECK_FALSE(RouteTranslator::percentDecodePathValue("%FF", out));     // not UTF-8
    CHECK_FALSE(RouteTranslator::percentDecodePathValue("%C0%AF", out));  // overlong '/'
}
