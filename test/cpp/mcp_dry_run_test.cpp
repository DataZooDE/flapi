#include <catch2/catch_test_macros.hpp>
#include <crow/json.h>

#include "mcp_dry_run.hpp"

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

} // namespace test
} // namespace flapi
