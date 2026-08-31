#include <catch2/catch_test_macros.hpp>

#include "config_manager.hpp"
#include "mcp_schema_builder.hpp"

namespace flapi {
namespace test {

namespace {

RequestFieldConfig makeField(const std::string& name,
                             const std::string& validator_type = "",
                             bool required = false) {
    RequestFieldConfig f;
    f.fieldName = name;
    f.required = required;
    if (!validator_type.empty()) {
        ValidatorConfig v;
        v.type = validator_type;
        f.validators.push_back(v);
    }
    return f;
}

// Parse the wvalue back through crow::json::load so we can assert on it.
crow::json::rvalue parse(const crow::json::wvalue& w) {
    return crow::json::load(w.dump());
}

} // namespace

TEST_CASE("MCPSchemaBuilder: empty field list yields empty object schema", "[mcp][schema]") {
    auto schema = parse(MCPSchemaBuilder::buildInputSchema({}));
    REQUIRE(schema["type"].s() == "object");
    REQUIRE(schema.has("properties"));
    REQUIRE_FALSE(schema.has("required"));
}

TEST_CASE("MCPSchemaBuilder: field with no validators is a string", "[mcp][schema]") {
    auto schema = parse(MCPSchemaBuilder::buildInputSchema({makeField("q")}));
    REQUIRE(schema["properties"]["q"]["type"].s() == "string");
}

TEST_CASE("MCPSchemaBuilder: validator types map to JSON Schema types", "[mcp][schema]") {
    SECTION("int -> integer") {
        auto s = parse(MCPSchemaBuilder::buildInputSchema({makeField("id", "int")}));
        REQUIRE(s["properties"]["id"]["type"].s() == "integer");
    }
    SECTION("double -> number") {
        auto s = parse(MCPSchemaBuilder::buildInputSchema({makeField("p", "double")}));
        REQUIRE(s["properties"]["p"]["type"].s() == "number");
    }
    SECTION("boolean -> boolean") {
        auto s = parse(MCPSchemaBuilder::buildInputSchema({makeField("b", "boolean")}));
        REQUIRE(s["properties"]["b"]["type"].s() == "boolean");
    }
    SECTION("date -> string + format date") {
        auto s = parse(MCPSchemaBuilder::buildInputSchema({makeField("d", "date")}));
        REQUIRE(s["properties"]["d"]["type"].s() == "string");
        REQUIRE(s["properties"]["d"]["format"].s() == "date");
    }
    SECTION("uuid -> string + format uuid") {
        auto s = parse(MCPSchemaBuilder::buildInputSchema({makeField("u", "uuid")}));
        REQUIRE(s["properties"]["u"]["format"].s() == "uuid");
    }
    SECTION("email -> string + format email") {
        auto s = parse(MCPSchemaBuilder::buildInputSchema({makeField("e", "email")}));
        REQUIRE(s["properties"]["e"]["format"].s() == "email");
    }
}

TEST_CASE("MCPSchemaBuilder: enum projects allowedValues", "[mcp][schema]") {
    RequestFieldConfig f;
    f.fieldName = "status";
    ValidatorConfig v;
    v.type = "enum";
    v.allowedValues = {"active", "inactive"};
    f.validators.push_back(v);

    auto s = parse(MCPSchemaBuilder::buildInputSchema({f}));
    auto values = s["properties"]["status"]["enum"];
    REQUIRE(values.size() == 2);
    REQUIRE(values[0].s() == "active");
    REQUIRE(values[1].s() == "inactive");
}

TEST_CASE("MCPSchemaBuilder: numeric min/max become minimum/maximum", "[mcp][schema]") {
    RequestFieldConfig f;
    f.fieldName = "n";
    ValidatorConfig v;
    v.type = "int";
    v.min = 1;
    v.max = 100;
    f.validators.push_back(v);

    auto s = parse(MCPSchemaBuilder::buildInputSchema({f}));
    REQUIRE(s["properties"]["n"]["minimum"].i() == 1);
    REQUIRE(s["properties"]["n"]["maximum"].i() == 100);
}

TEST_CASE("MCPSchemaBuilder: an explicit bound of 0 is preserved (not treated as unset)", "[mcp][schema]") {
    // Regression: 0 was mistaken for "unset" and dropped from the schema.
    RequestFieldConfig f;
    f.fieldName = "n";
    ValidatorConfig v;
    v.type = "int";
    v.min = 0;
    v.max = 0;
    f.validators.push_back(v);

    auto s = parse(MCPSchemaBuilder::buildInputSchema({f}));
    REQUIRE(s["properties"]["n"].has("minimum"));
    REQUIRE(s["properties"]["n"]["minimum"].i() == 0);
    REQUIRE(s["properties"]["n"].has("maximum"));
    REQUIRE(s["properties"]["n"]["maximum"].i() == 0);
}

TEST_CASE("MCPSchemaBuilder: string min/max become length bounds; regex becomes pattern", "[mcp][schema]") {
    RequestFieldConfig f;
    f.fieldName = "s";
    ValidatorConfig v;
    v.type = "string";
    v.min = 2;
    v.max = 20;
    v.regex = "^[a-z]+$";
    f.validators.push_back(v);

    auto s = parse(MCPSchemaBuilder::buildInputSchema({f}));
    REQUIRE(s["properties"]["s"]["minLength"].i() == 2);
    REQUIRE(s["properties"]["s"]["maxLength"].i() == 20);
    REQUIRE(s["properties"]["s"]["pattern"].s() == "^[a-z]+$");
}

TEST_CASE("MCPSchemaBuilder: required fields are listed", "[mcp][schema]") {
    auto s = parse(MCPSchemaBuilder::buildInputSchema({
        makeField("a", "int", /*required=*/true),
        makeField("b", "string", /*required=*/false),
    }));
    REQUIRE(s.has("required"));
    REQUIRE(s["required"].size() == 1);
    REQUIRE(s["required"][0].s() == "a");
}

TEST_CASE("MCPSchemaBuilder: default value is projected", "[mcp][schema]") {
    RequestFieldConfig f = makeField("limit", "int");
    f.defaultValue = "100";
    auto s = parse(MCPSchemaBuilder::buildInputSchema({f}));
    REQUIRE(s["properties"]["limit"]["default"].s() == "100");
}

TEST_CASE("MCPSchemaBuilder: mcp-header becomes x-mcp-header annotation", "[mcp][schema]") {
    RequestFieldConfig f = makeField("tenant", "string");
    f.mcp_header = "Tenant";
    auto s = parse(MCPSchemaBuilder::buildInputSchema({f}));
    REQUIRE(s["properties"]["tenant"]["x-mcp-header"].s() == "Tenant");

    // No annotation emitted when mcp-header is unset.
    auto s2 = parse(MCPSchemaBuilder::buildInputSchema({makeField("plain", "string")}));
    REQUIRE_FALSE(s2["properties"]["plain"].has("x-mcp-header"));
}

} // namespace test
} // namespace flapi
