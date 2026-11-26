// Review follow-ups to #276 and #282: what an endpoint load refuses, warns
// about, and how the populate fallback (a cache without template-file) judges
// the endpoint's own template.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include "../../src/include/config_manager.hpp"
#include "../../src/include/database_manager.hpp"
#include "../../src/include/endpoint_config_parser.hpp"
#include "../../src/include/extended_yaml_parser.hpp"
#include "test_utils.hpp"

using namespace flapi;
using namespace flapi::test;

namespace {

const char* kConfig = R"(
project-name: review
project-description: review
template:
  path: ./sqls
connections:
  app:
    properties:
      path: ./x.parquet
)";

std::string loadError(TempTestConfig& temp) {
    try {
        temp.createConfigManager();
    } catch (const std::exception& e) {
        return e.what();
    }
    return "";
}

std::string joined(const std::vector<std::string>& lines) {
    std::string out;
    for (const auto& line : lines) {
        out += line + "\n";
    }
    return out;
}

const std::string kCache = "cache:\n  enabled: true\n  table: items_cache\n";

}  // namespace

TEST_CASE("A write endpoint whose template is a single SELECT keeps the materialised fallback",
          "[endpoint_parser][cache][review]") {
    // What `flapii endpoints wizard` generates: POST + cache, no template-file,
    // a SELECT template. The refresh materialises the SELECT; nothing is written twice.
    TempTestConfig temp(kConfig, "review_post_select");
    temp.writeSqlTemplate("items.sql", "SELECT 1 AS id\n{{#params.id}}WHERE 1 = {{ params.id }}{{/params.id}}\n");
    temp.writeEndpoint("items.yaml", "url-path: /items\nmethod: POST\ntemplate-source: items.sql\n"
                                     "connection: [app]\n" + kCache);
    REQUIRE(loadError(temp).empty());
}

TEST_CASE("A cache without template-file over a data-modifying template is a load error naming it",
          "[endpoint_parser][cache][review]") {
    SECTION("POST INSERT: method and path, and the reader's template-file") {
        TempTestConfig temp(kConfig, "review_post_insert");
        temp.writeSqlTemplate("serve.sql", "SELECT * FROM cache.main.items_cache");
        temp.writeSqlTemplate("items_cache.sql",
                              "CREATE OR REPLACE TABLE {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS SELECT 1");
        temp.writeSqlTemplate("create.sql", "INSERT INTO items VALUES ({{ params.id }}, '{{{ params.name }}}')");
        // a_ loads before b_: the reader is known when the writer is checked.
        temp.writeEndpoint("a_items.yaml", "url-path: /items\ntemplate-source: serve.sql\nconnection: [app]\n" +
                                               kCache + "  template-file: items_cache.sql\n");
        temp.writeEndpoint("b_items_post.yaml", "url-path: /items\nmethod: POST\ntemplate-source: create.sql\n"
                                                "connection: [app]\n" + kCache);
        const auto error = loadError(temp);
        REQUIRE(error.find("POST /items: a write endpoint's cache needs template-file") != std::string::npos);
        REQUIRE(error.find("INSERT") != std::string::npos);
        REQUIRE(error.find("GET /items") != std::string::npos);
        REQUIRE(error.find("items_cache.sql") != std::string::npos);
    }
    SECTION("a read endpoint whose template deletes is refused too") {
        TempTestConfig temp(kConfig, "review_get_delete");
        temp.writeSqlTemplate("purge.sql", "DELETE FROM items WHERE id < 0; SELECT * FROM items");
        temp.writeEndpoint("purge.yaml", "url-path: /purge\ntemplate-source: purge.sql\nconnection: [app]\n" +
                                             kCache);
        const auto error = loadError(temp);
        REQUIRE(error.find("GET /purge:") != std::string::npos);
        REQUIRE(error.find("DELETE") != std::string::npos);
    }
}

TEST_CASE("The populate fallback judges the endpoint template with DuckDB's parser",
          "[database_manager][populate][review]") {
    const PopulateTarget target{"cache", "main", "items_cache"};
    using DM = DatabaseManager;
    REQUIRE_FALSE(DM::ServingScriptModification("SELECT 1", target).has_value());
    REQUIRE_FALSE(DM::ServingScriptModification("WITH x AS (SELECT 1) SELECT * FROM x", target).has_value());
    REQUIRE_FALSE(DM::ServingScriptModification("SET threads = 2; SELECT 1", target).has_value());
    REQUIRE_FALSE(DM::ServingScriptModification(
                      "CREATE OR REPLACE TABLE cache.main.items_cache AS SELECT 1", target).has_value());
    REQUIRE(DM::ServingScriptModification("INSERT INTO t VALUES (1)", target) == std::optional<std::string>("INSERT"));
    REQUIRE(DM::ServingScriptModification("SELECT 1; DELETE FROM t", target) == std::optional<std::string>("DELETE"));
    REQUIRE(DM::ServingScriptModification("UPDATE t SET a = 1", target) == std::optional<std::string>("UPDATE"));
    REQUIRE(DM::ServingScriptModification("COPY t TO 'x.csv'", target) == std::optional<std::string>("COPY"));
    REQUIRE(DM::ServingScriptModification("CREATE TABLE other AS SELECT 1", target) ==
            std::optional<std::string>("CREATE"));
    REQUIRE(DM::ServingScriptModification("MERGE INTO t USING s ON t.id = s.id WHEN MATCHED THEN DELETE", target)
                .has_value());
    // `SELECT ... 'DELETE'`: a string literal is not a statement.
    REQUIRE_FALSE(DM::ServingScriptModification("SELECT 'DELETE FROM x' AS s", target).has_value());
    // Unreadable: not run as a populate script either.
    REQUIRE(DM::ServingScriptModification("SELEKT 1", target).has_value());
}

TEST_CASE("A duplicate top-level endpoint key is reported; a duplicate auth is a security warning",
          "[endpoint_parser][unknown_keys][review]") {
    TempTestConfig temp(kConfig, "review_duplicate_auth");
    temp.writeSqlTemplate("one.sql", "SELECT 1");
    temp.writeEndpoint("common.yaml", "auth-prod:\n  enabled: true\n  type: basic\n  users:\n"
                                      "    - username: u\n      password: p\n");
    temp.writeEndpoint("e.yaml", "url-path: /e\ntemplate-source: one.sql\nconnection: [app]\n"
                                 "auth:\n  enabled: false\n"
                                 "{{include:auth-prod as auth from common.yaml}}\n");
    auto cm = temp.createConfigManager();
    const auto endpoint = cm->getEndpointForPath("/e");
    REQUIRE(endpoint);
    REQUIRE(endpoint->duplicate_keys == std::vector<std::string>{"auth"});
    const auto warnings = joined(cm->validateEndpointConfig(*endpoint).warnings);
    REQUIRE(warnings.find("SECURITY: endpoint key `auth` appears more than once") != std::string::npos);
    // The first one is what is read: the hazard the warning is for.
    REQUIRE_FALSE(endpoint->auth.enabled);
}

TEST_CASE("A file that looks like a mistyped endpoint is not skipped silently", "[endpoint_parser][review]") {
    TempTestConfig temp(kConfig, "review_would_be_endpoint");
    temp.writeSqlTemplate("one.sql", "SELECT 1");
    temp.writeEndpoint("typo.yaml", "urlpath: /typo\ntemplate-source: one.sql\nconnection: [app]\n");
    temp.writeEndpoint("mcp_typo.yaml", "mcp_tool:\n  name: t\n  description: d\ntemplate-source: one.sql\n");
    // A real fragment: no endpoint-shaped key.
    temp.writeEndpoint("fragment.yaml", "request:\n  - field-name: id\n    field-in: query\n");
    auto cm = temp.createConfigManager();
    const auto warnings = joined(cm->getLoadWarnings());
    REQUIRE(warnings.find("typo.yaml") != std::string::npos);
    REQUIRE(warnings.find("did you mean `url-path`?") != std::string::npos);
    REQUIRE(warnings.find("did you mean `mcp-tool`?") != std::string::npos);
    REQUIRE(warnings.find("fragment.yaml") == std::string::npos);
}

TEST_CASE("Include directive syntax: AS, spaces inside the braces, a missing section",
          "[extended_yaml_parser][include_as][review]") {
    TempTestConfig temp(kConfig, "review_include_syntax");
    temp.writeEndpoint("common.yaml", "auth-prod:\n  enabled: true\n");
    ExtendedYamlParser parser;
    const auto base = std::filesystem::path(temp.sqlsPath());

    auto spaced = parser.parseString("url-path: /x\n{{ include:auth-prod as auth from common.yaml }}\n", base);
    REQUIRE(spaced.success);
    REQUIRE(spaced.node["auth"]["enabled"].as<bool>());

    auto upper = parser.parseString("url-path: /x\n{{include:auth-prod AS auth from common.yaml}}\n", base);
    REQUIRE_FALSE(upper.success);
    REQUIRE(upper.error_message.find("{{include:SECTION as KEY from FILE}}") != std::string::npos);

    auto missing = parser.parseString("url-path: /x\n{{include:auth-dev as auth from common.yaml}}\n", base);
    REQUIRE_FALSE(missing.success);
    REQUIRE(missing.error_message.find("Section 'auth-dev' not found") != std::string::npos);
    REQUIRE(missing.error_message.find("common.yaml") != std::string::npos);
}
