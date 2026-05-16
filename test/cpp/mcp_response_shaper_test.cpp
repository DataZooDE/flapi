#include <catch2/catch_test_macros.hpp>
#include <crow/json.h>

#include "mcp_response_shaper.hpp"

namespace flapi {
namespace test {

namespace {

const std::string kThreeRowsJson = R"([
    {"id": 1, "name": "alice", "salary": 100},
    {"id": 2, "name": "bob",   "salary": 200},
    {"id": 3, "name": "carol", "salary": 300}
])";

} // namespace

TEST_CASE("MCPResponseShaper: default config leaves the data unchanged",
          "[security][mcp][response_shape]") {
    MCPResponseShaper shaper;
    ResponseShape config;  // all defaults
    auto out = shaper.shape(kThreeRowsJson, config);

    auto parsed = crow::json::load(out);
    REQUIRE(parsed);
    REQUIRE(parsed.size() == 3);
    REQUIRE(parsed[0]["name"].s() == std::string("alice"));
    REQUIRE(parsed[2]["salary"].i() == 300);
}

TEST_CASE("MCPResponseShaper: max_rows truncates the array",
          "[security][mcp][response_shape]") {
    MCPResponseShaper shaper;
    ResponseShape config;
    config.max_rows = 2;
    auto out = shaper.shape(kThreeRowsJson, config);

    auto parsed = crow::json::load(out);
    REQUIRE(parsed);
    REQUIRE(parsed.size() == 2);
    REQUIRE(parsed[0]["id"].i() == 1);
    REQUIRE(parsed[1]["id"].i() == 2);
}

TEST_CASE("MCPResponseShaper: redact_columns masks values in every row",
          "[security][mcp][response_shape]") {
    MCPResponseShaper shaper;
    ResponseShape config;
    config.redact_columns = {"salary"};
    auto out = shaper.shape(kThreeRowsJson, config);

    auto parsed = crow::json::load(out);
    REQUIRE(parsed);
    REQUIRE(parsed.size() == 3);
    for (size_t i = 0; i < parsed.size(); ++i) {
        REQUIRE(parsed[i]["salary"].s() == std::string("<redacted>"));
        // Non-redacted columns must survive untouched.
        REQUIRE_FALSE(parsed[i]["name"].s() == std::string("<redacted>"));
    }
}

TEST_CASE("MCPResponseShaper: redact + max_rows compose correctly",
          "[security][mcp][response_shape]") {
    MCPResponseShaper shaper;
    ResponseShape config;
    config.max_rows = 1;
    config.redact_columns = {"salary", "name"};
    auto out = shaper.shape(kThreeRowsJson, config);

    auto parsed = crow::json::load(out);
    REQUIRE(parsed);
    REQUIRE(parsed.size() == 1);
    REQUIRE(parsed[0]["name"].s() == std::string("<redacted>"));
    REQUIRE(parsed[0]["salary"].s() == std::string("<redacted>"));
    // Non-redacted column passes through.
    REQUIRE(parsed[0]["id"].i() == 1);
}

TEST_CASE("MCPResponseShaper: sample=true returns row count + column names only",
          "[security][mcp][response_shape]") {
    MCPResponseShaper shaper;
    ResponseShape config;
    config.sample = true;
    auto out = shaper.shape(kThreeRowsJson, config);

    auto parsed = crow::json::load(out);
    REQUIRE(parsed);
    REQUIRE(parsed.has("row_count"));
    REQUIRE(parsed["row_count"].i() == 3);
    REQUIRE(parsed.has("columns"));
    REQUIRE(parsed["columns"].size() == 3);
    REQUIRE_FALSE(parsed.has("rows"));
    REQUIRE(parsed.has("sampled"));
    REQUIRE(parsed["sampled"].b() == true);
}

TEST_CASE("MCPResponseShaper: sample wins over redact and max_rows",
          "[security][mcp][response_shape]") {
    // When sample is on we never emit row data, so the other knobs are
    // structurally inert. The test pins that contract.
    MCPResponseShaper shaper;
    ResponseShape config;
    config.sample = true;
    config.max_rows = 1;
    config.redact_columns = {"name"};
    auto out = shaper.shape(kThreeRowsJson, config);

    auto parsed = crow::json::load(out);
    REQUIRE(parsed);
    REQUIRE(parsed["row_count"].i() == 3);
    REQUIRE_FALSE(parsed.has("rows"));
}

TEST_CASE("MCPResponseShaper: non-array input is returned unchanged",
          "[security][mcp][response_shape]") {
    // Defensive: handlers that already produced a non-array payload (e.g.
    // dry-run results or empty wrapper objects) must not be corrupted.
    MCPResponseShaper shaper;
    ResponseShape config;
    config.max_rows = 10;
    auto out = shaper.shape(R"({"some":"object"})", config);

    auto parsed = crow::json::load(out);
    REQUIRE(parsed);
    REQUIRE(parsed["some"].s() == std::string("object"));
}

TEST_CASE("MCPResponseShaper: empty array stays empty",
          "[security][mcp][response_shape]") {
    MCPResponseShaper shaper;
    ResponseShape config;
    config.max_rows = 10;
    config.redact_columns = {"anything"};
    auto out = shaper.shape("[]", config);

    auto parsed = crow::json::load(out);
    REQUIRE(parsed);
    REQUIRE(parsed.size() == 0);
}

TEST_CASE("MCPResponseShaper: max_rows of 0 truncates everything",
          "[security][mcp][response_shape]") {
    MCPResponseShaper shaper;
    ResponseShape config;
    config.max_rows = 0;
    auto out = shaper.shape(kThreeRowsJson, config);

    auto parsed = crow::json::load(out);
    REQUIRE(parsed);
    REQUIRE(parsed.size() == 0);
}

TEST_CASE("MCPResponseShaper: redact-columns referring to absent column is a no-op",
          "[security][mcp][response_shape]") {
    // Misconfiguration shouldn't crash or alter the payload structurally.
    MCPResponseShaper shaper;
    ResponseShape config;
    config.redact_columns = {"not_present"};
    auto out = shaper.shape(kThreeRowsJson, config);

    auto parsed = crow::json::load(out);
    REQUIRE(parsed);
    REQUIRE(parsed.size() == 3);
    REQUIRE(parsed[0]["name"].s() == std::string("alice"));
}

} // namespace test
} // namespace flapi
