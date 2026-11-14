// Regressions for #211 cluster E2: method-aware config-service addressing,
// reload by file, duplicate MCP names, resources/read validation and shaping,
// the prepared preview, and the description-scanner message.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <fstream>
#include <random>

#include "config_manager.hpp"
#include "config_service.hpp"
#include "mcp_description_scanner.hpp"
#include "mcp_dry_run.hpp"
#include "mcp_route_handlers.hpp"
#include "mcp_tool_handler.hpp"

using namespace flapi;
using Catch::Matchers::ContainsSubstring;

namespace {

struct Project {
    std::filesystem::path dir;
    std::filesystem::path sqls;

    Project() {
        std::mt19937_64 rng{std::random_device{}()};
        dir = std::filesystem::temp_directory_path() / ("flapi_e2_" + std::to_string(rng()));
        sqls = dir / "sqls";
        std::filesystem::create_directories(sqls);
        write("flapi.yaml",
              "project-name: e2\n"
              "template:\n"
              "  path: ./sqls\n"
              "connections:\n"
              "  mem:\n"
              "    properties:\n"
              "      db: ':memory:'\n");
        write("sqls/q.sql", "SELECT 1 AS x\n");
    }
    ~Project() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    void write(const std::string& rel, const std::string& content) const {
        std::ofstream out(dir / rel, std::ios::trunc);
        out << content;
    }
    std::shared_ptr<ConfigManager> load() const {
        auto cm = std::make_shared<ConfigManager>(dir / "flapi.yaml");
        cm->loadConfig();
        return cm;
    }
};

std::string restFile(const std::string& method, const std::string& description) {
    return "url-path: /orders\n"
           "method: " + method + "\n"
           "template-source: q.sql\n"
           "connection: [mem]\n"
           "mcp-tool:\n"
           "  name: orders_" + method + "\n"
           "  description: " + description + "\n";
}

}  // namespace

TEST_CASE("A slug shared by GET and POST is ambiguous without ?method=", "[config_service][e2][regression]") {
    Project p;
    p.write("sqls/orders_get.yaml", restFile("GET", "list orders"));
    p.write("sqls/orders_post.yaml", restFile("POST", "create an order"));
    auto cm = p.load();
    REQUIRE(cm->getEndpoints()->size() == 2);

    const auto ambiguous = ResolveSlugTarget(*cm, "-orders", nullptr);
    CHECK_FALSE(ambiguous.ok);
    CHECK(ambiguous.status == 400);
    CHECK_THAT(ambiguous.error, ContainsSubstring("?method=GET|POST"));

    const auto post = ResolveSlugTarget(*cm, "-orders", "post");
    REQUIRE(post.ok);
    CHECK(post.key == "POST /orders");
    CHECK(cm->getEndpointForKey(post.key)->method == "POST");

    const auto missing = ResolveSlugTarget(*cm, "-orders", "DELETE");
    CHECK(missing.status == 404);
}

TEST_CASE("Reloading the POST file changes POST, not GET", "[config_service][e2][regression]") {
    Project p;
    p.write("sqls/orders_get.yaml", restFile("GET", "list orders"));
    p.write("sqls/orders_post.yaml", restFile("POST", "create an order"));
    auto cm = p.load();

    p.write("sqls/orders_post.yaml", restFile("POST", "create an order v2"));
    REQUIRE(cm->reloadEndpointConfig("POST /orders"));
    CHECK(cm->getEndpointForKey("POST /orders")->mcp_tool->description == "create an order v2");
    CHECK(cm->getEndpointForKey("GET /orders")->mcp_tool->description == "list orders");
    CHECK(cm->getEndpoints()->size() == 2);

    // The list keeps both, keyed "METHOD /path" because they share the path.
    const auto list = crow::json::load(cm->getEndpointsConfig().dump());
    CHECK(list.has("GET /orders"));
    CHECK(list.has("POST /orders"));

    // removeEndpoint removes exactly one.
    const auto post = cm->getEndpointForKey("POST /orders");
    REQUIRE(cm->removeEndpoint(*post));
    REQUIRE(cm->getEndpoints()->size() == 1);
    CHECK(cm->getEndpoints()->front().method == "GET");
}

TEST_CASE("reloadEndpointFile loads a file that is not loaded yet", "[config_service][e2][regression]") {
    Project p;
    p.write("sqls/orders_get.yaml", restFile("GET", "list orders"));
    auto cm = p.load();
    REQUIRE(cm->getEndpoints()->size() == 1);

    p.write("sqls/fresh.yaml", "url-path: /fresh\ntemplate-source: q.sql\nconnection: [mem]\n");
    auto loaded = cm->reloadEndpointFile(p.sqls / "fresh.yaml");
    REQUIRE(loaded.ok);
    CHECK(loaded.loaded_new);
    CHECK(loaded.status == 201);
    REQUIRE(loaded.endpoints.size() == 1);
    CHECK(loaded.endpoints.front() == "GET /fresh");

    // A second call reloads it in place.
    auto again = cm->reloadEndpointFile(p.sqls / "fresh.yaml");
    REQUIRE(again.ok);
    CHECK_FALSE(again.loaded_new);
    CHECK(cm->getEndpoints()->size() == 2);

    // A non-endpoint YAML is refused with a reason, not loaded.
    p.write("sqls/shared.yaml", "auth:\n  enabled: true\n");
    auto shared = cm->reloadEndpointFile(p.sqls / "shared.yaml");
    CHECK_FALSE(shared.ok);
    CHECK(shared.status == 400);
}

TEST_CASE("Duplicate MCP tool names are a load error naming both files", "[config_manager][e2][regression]") {
    Project p;
    p.write("sqls/a.yaml", "template-source: q.sql\nconnection: [mem]\nmcp-tool:\n  name: northwind_order\n  description: a\n");
    p.write("sqls/b.yaml", "template-source: q.sql\nconnection: [mem]\nmcp-tool:\n  name: northwind_order\n  description: b\n");
    auto cm = std::make_shared<ConfigManager>(p.dir / "flapi.yaml");
    try {
        cm->loadConfig();
        FAIL("two tools named northwind_order loaded");
    } catch (const std::exception& e) {
        const std::string what = e.what();
        CHECK_THAT(what, ContainsSubstring("Duplicate MCP tool name 'northwind_order'"));
        CHECK_THAT(what, ContainsSubstring("a.yaml"));
        CHECK_THAT(what, ContainsSubstring("b.yaml"));
    }
}

TEST_CASE("A tool and a resource may share a name", "[config_manager][e2]") {
    Project p;
    p.write("sqls/a.yaml", "template-source: q.sql\nconnection: [mem]\nmcp-tool:\n  name: orders\n  description: a\n");
    p.write("sqls/b.yaml", "template-source: q.sql\nconnection: [mem]\nmcp-resource:\n  name: orders\n  description: b\n");
    auto cm = p.load();
    CHECK(cm->getEndpoints()->size() == 2);
}

TEST_CASE("DecodeSlugSegment decodes only %3A", "[config_service][e2][regression]") {
    CHECK(DecodeSlugSegment("-products-%3Aid") == "-products-:id");
    CHECK(DecodeSlugSegment("-products-%3aid") == "-products-:id");
    CHECK(DecodeSlugSegment("-a%2Fb") == "-a%2Fb");
    CHECK(DecodeSlugSegment("%3") == "%3");
}

TEST_CASE("validateMcpParams applies defaults and runs the validators", "[mcp][e2][regression]") {
    EndpointConfig ep;
    RequestFieldConfig id;
    id.fieldName = "id";
    id.fieldIn = "path";
    ValidatorConfig v;
    v.type = "string";
    v.regex = "^[A-Z]{5}$";
    id.validators.push_back(v);
    ep.request_fields.push_back(id);
    RequestFieldConfig limit;
    limit.fieldName = "limit";
    limit.fieldIn = "query";
    limit.defaultValue = "10";
    ep.request_fields.push_back(limit);

    std::map<std::string, std::string> ok{{"id", "ALFKI"}};
    CHECK(validateMcpParams(ep, ok).empty());
    CHECK(ok.at("limit") == "10");

    std::map<std::string, std::string> injected{{"id", "x' OR '1'='1"}};
    CHECK_THAT(validateMcpParams(ep, injected), ContainsSubstring("id"));

    std::map<std::string, std::string> unknown{{"id", "ALFKI"}, {"other", "1"}};
    CHECK_THAT(validateMcpParams(ep, unknown), ContainsSubstring("other"));
}

TEST_CASE("resources/read shaping: own block, else the tool's, else the default cap", "[mcp][e2][regression]") {
    EndpointConfig ep;
    ep.mcp_resource = EndpointConfig::MCPResourceInfo{};
    auto shape = resourceResponseShape(ep);
    REQUIRE(shape.max_rows.has_value());
    CHECK(*shape.max_rows == EndpointConfig::MCPResourceInfo::kDefaultMaxRows);

    ep.mcp_tool = EndpointConfig::MCPToolInfo{};
    ep.mcp_tool->response.max_rows = 25;
    ep.mcp_tool->response.redact_columns = {"email"};
    shape = resourceResponseShape(ep);
    CHECK(*shape.max_rows == 25);
    CHECK(shape.redact_columns == std::vector<std::string>{"email"});

    ep.mcp_resource->response.max_rows = 5;
    ep.mcp_resource->response.redact_columns = std::vector<std::string>{};
    shape = resourceResponseShape(ep);
    CHECK(*shape.max_rows == 5);
    CHECK(shape.redact_columns.empty());
}

TEST_CASE("The dry-run preview types parameters and lists bindings", "[mcp][dry_run][e2][regression]") {
    RequestFieldConfig limit;
    limit.fieldName = "limit";
    ValidatorConfig v;
    v.type = "int";
    limit.validators.push_back(v);
    const std::vector<RequestFieldConfig> fields{limit};

    std::vector<PreparedBindingSpec> bindings(2);
    bindings[0].field_name = "limit";
    bindings[0].type = SqlParameterType::Integer;
    bindings[1].field_name = "missing";

    const TemplateSecrets none;
    const auto payload = crow::json::load(MCPDryRun::formatResult(
        "t", "SELECT ? , ?", {{"limit", "3"}}, bindings, fields, none));
    REQUIRE(payload);
    CHECK(payload["parameters"]["limit"].t() == crow::json::type::Number);
    CHECK(payload["parameters"]["limit"].i() == 3);
    REQUIRE(payload["bindings"].size() == 2);
    CHECK(payload["bindings"][0]["position"].i() == 1);
    CHECK(payload["bindings"][0]["type"].s() == "integer");
    CHECK(payload["bindings"][0]["value"].i() == 3);
    CHECK(payload["bindings"][1]["value"].t() == crow::json::type::Null);
}

TEST_CASE("The description scanner names the phrase and its offset", "[mcp][e2][regression]") {
    MCPDescriptionScanner scanner;
    const auto issues = scanner.scan("Reads the billing system: totals");
    REQUIRE(issues.size() == 1);
    CHECK_THAT(issues[0].message, ContainsSubstring("'system:'"));
    CHECK_THAT(issues[0].message, ContainsSubstring("offset 18"));
}
