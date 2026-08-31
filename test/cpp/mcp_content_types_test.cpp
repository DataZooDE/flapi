#include <catch2/catch_test_macros.hpp>

#include "mcp_content_types.hpp"

namespace flapi {
namespace test {

namespace {

crow::json::rvalue parse(const crow::json::wvalue& w) {
    return crow::json::load(w.dump());
}

} // namespace

TEST_CASE("ContentResponse: plain success has content and no isError", "[mcp][content]") {
    mcp::ContentResponse r;
    r.addText("hello");
    auto j = parse(r.toJson());
    REQUIRE(j.has("content"));
    REQUIRE(j["content"].size() == 1);
    REQUIRE_FALSE(j.has("isError"));
    REQUIRE_FALSE(j.has("structuredContent"));
}

TEST_CASE("ContentResponse: setError emits isError:true", "[mcp][content]") {
    mcp::ContentResponse r;
    r.addText("bad arguments: id must be > 0");
    r.setError(true);
    auto j = parse(r.toJson());
    REQUIRE(j.has("isError"));
    REQUIRE(j["isError"].b() == true);
    // The error text is still delivered as a content block so the model sees it.
    REQUIRE(j["content"][0]["text"].s() == "bad arguments: id must be > 0");
}

TEST_CASE("ContentResponse: structuredContent is attached alongside content", "[mcp][content]") {
    mcp::ContentResponse r;
    r.addText("[{\"x\":1}]");
    crow::json::wvalue structured;
    structured["row_count"] = 1;
    r.setStructuredContent(std::move(structured));

    auto j = parse(r.toJson());
    REQUIRE(j.has("content"));
    REQUIRE(j.has("structuredContent"));
    REQUIRE(j["structuredContent"]["row_count"].i() == 1);
    REQUIRE_FALSE(j.has("isError"));
}

} // namespace test
} // namespace flapi
