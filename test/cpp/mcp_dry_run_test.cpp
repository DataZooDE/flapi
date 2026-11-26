#include <catch2/catch_test_macros.hpp>
#include <crow/json.h>

#include "mcp_dry_run.hpp"
#include "mcp_route_handlers.hpp"

namespace flapi {
namespace test {

TEST_CASE("MCPDryRun::extractFlag: missing key yields false and no change",
          "[security][mcp][dryrun]") {
    crow::json::wvalue args;
    args["id"] = 42;

    bool extracted = MCPDryRun::extractFlag(args);

    REQUIRE_FALSE(extracted);
    // The original argument must still be present.
    auto dumped = args.dump();
    REQUIRE(dumped.find("\"id\":42") != std::string::npos);
}

TEST_CASE("MCPDryRun::extractFlag: _dryRun=true is consumed and returns true",
          "[security][mcp][dryrun]") {
    crow::json::wvalue args;
    args["id"] = 42;
    args["_dryRun"] = true;

    bool extracted = MCPDryRun::extractFlag(args);

    REQUIRE(extracted);
    auto dumped = args.dump();
    // The flag must be stripped so downstream validators do not see it.
    REQUIRE(dumped.find("_dryRun") == std::string::npos);
    // Other arguments must survive untouched.
    REQUIRE(dumped.find("\"id\":42") != std::string::npos);
}

TEST_CASE("MCPDryRun::extractFlag: _dryRun=false is consumed and returns false",
          "[security][mcp][dryrun]") {
    crow::json::wvalue args;
    args["_dryRun"] = false;

    bool extracted = MCPDryRun::extractFlag(args);

    REQUIRE_FALSE(extracted);
    auto dumped = args.dump();
    REQUIRE(dumped.find("_dryRun") == std::string::npos);
}

TEST_CASE("MCPDryRun::extractFlag: non-boolean _dryRun is rejected and stripped",
          "[security][mcp][dryrun]") {
    // A string or numeric _dryRun is treated as not-set; we still strip the
    // key so it never reaches the validator. This is conservative: only an
    // explicit boolean true engages dry-run.
    crow::json::wvalue args;
    args["_dryRun"] = "yes";

    bool extracted = MCPDryRun::extractFlag(args);

    REQUIRE_FALSE(extracted);
    auto dumped = args.dump();
    REQUIRE(dumped.find("_dryRun") == std::string::npos);
}

TEST_CASE("MCPDryRun::formatResult: produces JSON with dry_run, tool, sql, params",
          "[security][mcp][dryrun]") {
    std::map<std::string, std::string> params = {
        {"id", "42"},
        {"region", "EU"},
    };
    std::string rendered = "SELECT * FROM customers WHERE id = 42 AND region = 'EU'";

    std::string payload = MCPDryRun::formatResult("customer_lookup", rendered, params);

    auto parsed = crow::json::load(payload);
    REQUIRE(parsed);
    REQUIRE(parsed["dry_run"].b() == true);
    REQUIRE(parsed["tool_name"].s() == std::string("customer_lookup"));
    REQUIRE(parsed["rendered_sql"].s() == rendered);
    // Parameters must round-trip as a JSON object keyed by name.
    REQUIRE(parsed["parameters"]["id"].s() == std::string("42"));
    REQUIRE(parsed["parameters"]["region"].s() == std::string("EU"));
}

TEST_CASE("MCPDryRun::formatResult: empty parameter map still emits a parameters object",
          "[security][mcp][dryrun]") {
    std::string payload = MCPDryRun::formatResult(
        "no_arg_tool", "SELECT 1", /*parameters=*/{});

    auto parsed = crow::json::load(payload);
    REQUIRE(parsed);
    REQUIRE(parsed["dry_run"].b() == true);
    REQUIRE(parsed.has("parameters"));
}

TEST_CASE("toolPayloadOf: a dry-run result is a summary, not rows",
          "[mcp][dryrun][issue275]") {
    // The tool handler marks a dry run with metadata dry_run=true; its preview
    // object must not be wrapped as rows (or padded with rows: []).
    REQUIRE(toolPayloadOf({{"dry_run", "true"}}) == ToolPayload::Summary);
    REQUIRE(toolPayloadOf({{"tool_name", "x"}}) == ToolPayload::Rows);
}

TEST_CASE("toolSuccessResultJson: a dry-run preview is the structuredContent, with no rows",
          "[mcp][dryrun][issue275]") {
    const std::string preview = MCPDryRun::formatResult("customer_lookup", "SELECT 1", {{"id", "42"}});
    const auto out = crow::json::load(toolSuccessResultJson(preview, toolPayloadOf({{"dry_run", "true"}})));
    REQUIRE(out);
    REQUIRE(out["content"][0]["text"].s() == preview);
    const auto& structured = out["structuredContent"];
    REQUIRE(structured["dry_run"].b() == true);
    REQUIRE_FALSE(structured.has("rows"));
    REQUIRE_FALSE(structured.has("row_count"));
}

TEST_CASE("toolSuccessResultJson: rows go under structuredContent.rows with a row_count",
          "[mcp][issue275]") {
    const std::string rows = R"([{"zeta":1,"alpha":711.56}])";
    const std::string out = toolSuccessResultJson(rows, ToolPayload::Rows);
    REQUIRE(out.find(R"("structuredContent":{"rows":[{"zeta":1,"alpha":711.56}],"row_count":1})") !=
            std::string::npos);
}

TEST_CASE("MCPDryRun::formatResult: the injected __auth_* identity is not echoed",
          "[security][mcp][dryrun]") {
    const std::map<std::string, std::string> params = {
        {"id", "42"},
        {"__auth_username", "alice"},
        {"__auth_roles", "analyst"},
        {"__auth_type", "bearer"},
        {"__auth_authenticated", "true"},
    };
    for (const auto& payload : {MCPDryRun::formatResult("t", "SELECT 1", params),
                                MCPDryRun::formatResult("t", "SELECT 1", params, {}, {}, TemplateSecrets{})}) {
        REQUIRE(payload.find("__auth_") == std::string::npos);
        const auto parsed = crow::json::load(payload);
        REQUIRE(parsed);
        REQUIRE(parsed["parameters"].has("id"));
        REQUIRE(parsed["parameters"].size() == 1);
    }
}

} // namespace test
} // namespace flapi
