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
                            "/a/b:c", "/a b", "/a%3Aid"}) {
        INFO(bad);
        CHECK_FALSE(RouteTranslator::urlPathProblem(bad).empty());
    }
    CHECK(RouteTranslator::urlPathProblem("/customers/{cid}").find("`:cid`") != std::string::npos);
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
    SECTION("the more specific route is the protected one") {
        flapi::test::TempTestConfig temp(kConfig, "overlap_stricter");
        temp.writeSqlTemplate("a.sql", "SELECT 1 AS n\n");
        temp.writeEndpoint("a.yaml", endpointYaml("/a/:x/b", "GET", true));
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
    const auto protected_file = temp.writeEndpoint("a.yaml", endpointYaml("/customers/7", "GET", true));
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
