#include <catch2/catch_test_macros.hpp>
#include <crow/json.h>

#include "mcp_response_shaper.hpp"
#include "query_executor.hpp"

namespace flapi {
namespace test {

namespace {

const std::string kThreeRowsJson = R"([
    {"id": 1, "name": "alice", "salary": 100},
    {"id": 2, "name": "bob",   "salary": 200},
    {"id": 3, "name": "carol", "salary": 300}
])";

// A QueryResult as the database manager builds one: the rows in `data` and
// the column names, in SELECT order, in `columns`.
QueryResult makeResult(const std::string& rows_json, const std::vector<std::string>& columns) {
    QueryResult result;
    result.data = crow::json::wvalue(crow::json::load(rows_json));
    result.columns = columns;
    return result;
}

QueryResult threeRows() {
    return makeResult(kThreeRowsJson, {"id", "name", "salary"});
}

// Columns whose SELECT order is neither alphabetical nor what a hash map
// iterates, and doubles a 21-digit printer turns into 711.55999999999994543.
QueryResult orderedRows() {
    QueryResult result;
    result.columns = {"zeta", "alpha", "mid", "beta"};
    result.data = crow::json::wvalue::list();
    const double alphas[] = {711.56, 2.675};
    const double betas[] = {0.1, 1.1};
    const char* mids[] = {"a", "b"};
    for (int i = 0; i < 2; ++i) {
        crow::json::wvalue row;
        row["zeta"] = static_cast<int64_t>(i + 1);
        row["alpha"] = alphas[i];
        row["mid"] = std::string(mids[i]);
        row["beta"] = betas[i];
        result.data[i] = std::move(row);
    }
    return result;
}

} // namespace

TEST_CASE("MCPResponseShaper: default config leaves the data unchanged",
          "[security][mcp][response_shape]") {
    MCPResponseShaper shaper;
    ResponseShape config;  // all defaults
    auto out = shaper.shape(threeRows(), config);

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
    auto out = shaper.shape(threeRows(), config);

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
    auto out = shaper.shape(threeRows(), config);

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
    auto out = shaper.shape(threeRows(), config);

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
    auto out = shaper.shape(threeRows(), config);

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
    auto out = shaper.shape(threeRows(), config);

    auto parsed = crow::json::load(out);
    REQUIRE(parsed);
    REQUIRE(parsed["row_count"].i() == 3);
    REQUIRE_FALSE(parsed.has("rows"));
}

TEST_CASE("MCPResponseShaper: empty result stays empty",
          "[security][mcp][response_shape]") {
    MCPResponseShaper shaper;
    ResponseShape config;
    config.max_rows = 10;
    config.redact_columns = {"anything"};
    auto out = shaper.shape(makeResult("[]", {"anything"}), config);

    REQUIRE(out == "[]");
}

TEST_CASE("MCPResponseShaper: max_rows of 0 truncates everything",
          "[security][mcp][response_shape]") {
    MCPResponseShaper shaper;
    ResponseShape config;
    config.max_rows = 0;
    auto out = shaper.shape(threeRows(), config);

    REQUIRE(out == "[]");
}

TEST_CASE("MCPResponseShaper: redact-columns referring to absent column is a no-op",
          "[security][mcp][response_shape]") {
    // Misconfiguration shouldn't crash or alter the payload structurally.
    MCPResponseShaper shaper;
    ResponseShape config;
    config.redact_columns = {"not_present"};
    auto out = shaper.shape(threeRows(), config);

    auto parsed = crow::json::load(out);
    REQUIRE(parsed);
    REQUIRE(parsed.size() == 3);
    REQUIRE(parsed[0]["name"].s() == std::string("alice"));
}

// ---- #275: column order and number precision -------------------------------

TEST_CASE("MCPResponseShaper: redacted rows keep the SELECT column order and exact doubles",
          "[security][mcp][response_shape][issue275]") {
    MCPResponseShaper shaper;
    ResponseShape config;
    config.redact_columns = {"mid"};
    auto out = shaper.shape(orderedRows(), config);

    REQUIRE(out ==
            R"([{"zeta":1,"alpha":711.56,"mid":"<redacted>","beta":0.1},)"
            R"({"zeta":2,"alpha":2.675,"mid":"<redacted>","beta":1.1}])");
}

TEST_CASE("MCPResponseShaper: capped rows keep the SELECT column order and exact doubles",
          "[security][mcp][response_shape][issue275]") {
    MCPResponseShaper shaper;
    ResponseShape config;
    config.max_rows = 1;
    auto out = shaper.shape(orderedRows(), config);

    REQUIRE(out == R"([{"zeta":1,"alpha":711.56,"mid":"a","beta":0.1}])");
}

TEST_CASE("MCPResponseShaper: sample lists the columns in SELECT order",
          "[security][mcp][response_shape][issue275]") {
    MCPResponseShaper shaper;
    ResponseShape config;
    config.sample = true;
    auto out = shaper.shape(orderedRows(), config);

    REQUIRE(out == R"({"sampled":true,"row_count":2,"columns":["zeta","alpha","mid","beta"]})");
}

TEST_CASE("MCPResponseShaper: a streamed result cannot be shaped silently",
          "[security][mcp][response_shape][issue275]") {
    // Rows that went to a sink are only text; redaction must never be skipped
    // because the shaper had no rows to look at.
    QueryResult streamed;
    streamed.streamed = true;
    streamed.streamed_json = R"([{"salary":1}])";
    streamed.columns = {"salary"};
    ResponseShape config;
    config.redact_columns = {"salary"};
    REQUIRE_THROWS(MCPResponseShaper().shape(streamed, config));
}

} // namespace test
} // namespace flapi
