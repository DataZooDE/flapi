// Regressions for #211 cluster E1: Mustache balance checking (crash/hang),
// number validators, JSON number serialisation, row column order.
#include <catch2/catch_test_macros.hpp>

#include <random>
#include <string>
#include <vector>

#include <crow.h>

#include "config_manager.hpp"
#include "mcp_schema_builder.hpp"
#include "mustache_syntax_check.hpp"
#include "query_executor.hpp"
#include "request_validator.hpp"
#include "sql_template_processor.hpp"

namespace flapi {
namespace test {

namespace {

std::string messageOf(const std::string& body) {
    const auto error = CheckMustacheSyntax(body);
    return error ? error->message : std::string();
}

} // namespace

TEST_CASE("CheckMustacheSyntax rejects what crow would crash or hang on", "[mustache][e1]") {
    SECTION("a stray closing tag (crow: back() on an empty vector, SEGFAULT)") {
        const auto error = CheckMustacheSyntax("SELECT 1\n{{/x}}");
        REQUIRE(error);
        REQUIRE(error->line == 2);
        REQUIRE(error->column == 1);
        REQUIRE(error->message.find("never opened") != std::string::npos);
    }
    SECTION("an unclosed section (crow: jump target 0, infinite loop)") {
        const auto error = CheckMustacheSyntax("SELECT *\nFROM t\nWHERE 1=1 {{#params.x}} AND a = 1");
        REQUIRE(error);
        REQUIRE(error->line == 3);
        REQUIRE(error->message.find("never closed") != std::string::npos);
        REQUIRE(error->message.find("{{/params.x}}") != std::string::npos);
    }
    SECTION("an unclosed inverted section") {
        REQUIRE(messageOf("{{^x}} y").find("never closed") != std::string::npos);
    }
    SECTION("crossed sections") {
        const auto error = CheckMustacheSyntax("{{#a}}{{#b}}{{/a}}{{/b}}");
        REQUIRE(error);
        REQUIRE(error->message.find("does not match") != std::string::npos);
    }
    SECTION("a mismatched name") {
        REQUIRE(CheckMustacheSyntax("{{#a}}x{{/b}}"));
    }
    SECTION("triple braces closed by two") {
        REQUIRE(messageOf("'{{{ params.x }}'").find("}}}") != std::string::npos);
        REQUIRE(CheckMustacheSyntax("{{{x}}"));
    }
    SECTION("an unterminated tag") {
        REQUIRE(messageOf("SELECT {{ params.x").find("never closed") != std::string::npos);
    }
    SECTION("empty tags and nameless sections") {
        REQUIRE(CheckMustacheSyntax("{{}}"));
        REQUIRE(CheckMustacheSyntax("{{ }}"));
        REQUIRE(CheckMustacheSyntax("{{#}}{{/}}"));
        REQUIRE(CheckMustacheSyntax("{{# }}x{{/ }}"));
        REQUIRE(CheckMustacheSyntax("{{{ }}}"));
    }
    SECTION("partials and set-delimiter tags are not supported") {
        REQUIRE(messageOf("{{> other}}").find("partials") != std::string::npos);
        REQUIRE(messageOf("{{=<% %>=}}").find("set-delimiter") != std::string::npos);
    }
}

TEST_CASE("CheckMustacheSyntax accepts every shape flAPI templates use", "[mustache][e1]") {
    const std::vector<std::string> good = {
        "SELECT 1",
        "",
        "SELECT * FROM t WHERE 1=1\n{{#params.id}}\n  AND id = {{ params.id }}\n{{/params.id}}",
        "LIMIT {{#params.limit}}{{ params.limit }}{{/params.limit}}{{^params.limit}}100{{/params.limit}}",
        "SELECT '{{{ conn.path }}}' {{! a comment with }} inside? no: }}",
        "{{#a}}{{#b}}{{^c}}x{{/c}}{{/b}}{{/a}}",
        "{{&params.raw}} {{ . }}",
        "INSERT INTO {{cache.catalog}}.{{cache.schema}}.{{cache.table}} SELECT 1",
        "SELECT '{\"a\": {\"b\": 1}}'::JSON",
    };
    for (const auto& body : good) {
        INFO(body);
        REQUIRE_FALSE(CheckMustacheSyntax(body));
    }
}

TEST_CASE("FormatMustacheSyntaxError names the file and line", "[mustache][e1]") {
    const auto error = CheckMustacheSyntax("a\nb\n  {{/x}}");
    REQUIRE(error);
    REQUIRE(FormatMustacheSyntaxError(*error, "sqls/x.sql").rfind("sqls/x.sql:3:3: ", 0) == 0);
    REQUIRE_THROWS_AS(RequireValidMustacheSyntax("{{#x}}", "t.sql"), std::invalid_argument);
}

TEST_CASE("SQLTemplateProcessor refuses an unbalanced template instead of crashing", "[mustache][e1]") {
    SQLTemplateProcessor processor(nullptr);
    crow::mustache::context ctx;
    ctx["params"]["y"] = "1";
    // Both used to take the process down: SIGSEGV, and an endless loop that
    // grew memory without bound (x is absent from the context).
    REQUIRE_THROWS_AS(processor.processTemplate("SELECT 1 {{/x}}", ctx), std::invalid_argument);
    REQUIRE_THROWS_AS(processor.processTemplate("SELECT 1 {{#params.x}} AND 2", ctx), std::invalid_argument);
    REQUIRE(processor.processTemplate("SELECT {{#params.y}}{{ params.y }}{{/params.y}}", ctx) == "SELECT 1");
}

TEST_CASE("Fuzz: a template the checker accepts always compiles and renders", "[mustache][e1][fuzz]") {
    const std::vector<std::string> tokens = {
        "{{#a}}", "{{/a}}", "{{^a}}", "{{#b}}", "{{/b}}", "{{^b}}", "{{#p.x}}", "{{/p.x}}",
        "{{a}}", "{{ b }}", "{{{a}}}", "{{{a}}", "{{&a}}", "{{!c}}", "{{>p}}", "{{=<% %>=}}",
        "{{", "}}", "}}}", "{", "}", "{{/}}", "{{#}}", "{{ }}", "{{.}}", "{{p.x}}",
        "x", " ", "\n", "  ", "SELECT", "'", "{{#a }}", "{{/ a}}",
    };
    std::mt19937 rng(211);
    std::uniform_int_distribution<std::size_t> pick(0, tokens.size() - 1);
    std::uniform_int_distribution<int> length(0, 14);

    // Contexts covering absent, string, object and false values: the hang
    // needed an ABSENT section name, the crash any render at all.
    std::vector<crow::mustache::context> contexts(4);
    contexts[1]["a"] = "1";
    contexts[2]["a"] = "1";
    contexts[2]["b"] = "2";
    contexts[2]["p"]["x"] = "3";
    contexts[3]["a"] = false;
    contexts[3]["b"] = true;

    int accepted = 0;
    for (int iteration = 0; iteration < 20000; ++iteration) {
        std::string body;
        const int n = length(rng);
        for (int i = 0; i < n; ++i) {
            body += tokens[pick(rng)];
        }
        if (CheckMustacheSyntax(body)) {
            continue;   // never handed to crow
        }
        ++accepted;
        INFO("template: " << body);
        // At least as strict as crow: no parse error crow would raise.
        REQUIRE_NOTHROW(crow::mustache::compile(body));
        auto compiled = crow::mustache::compile(body);
        for (const auto& ctx : contexts) {
            try {
                const std::string out = compiled.render_string(ctx);
                // Rendering can only drop or substitute; a template of at most
                // 14 tokens over 1-character values cannot grow much.
                REQUIRE(out.size() <= body.size() + 64);
            } catch (const std::exception&) {
                // A type crow cannot render ({{.}} on an object) throws; that
                // is an error for the request, not a crash.
            }
        }
    }
    // The generator must actually exercise the accept path.
    REQUIRE(accepted > 1000);
}

TEST_CASE("number validator parses and enforces min/max", "[validator][e1]") {
    RequestValidator validator;
    RequestFieldConfig field;
    field.fieldName = "price";
    ValidatorConfig v;
    v.type = "number";
    v.minNumber = 0.5;
    v.maxNumber = 99.99;
    field.validators.push_back(v);

    auto check = [&](const std::string& value) {
        std::map<std::string, std::string> params{{"price", value}};
        return validator.validateRequestParameters({field}, params);
    };
    REQUIRE(check("9.99").empty());
    REQUIRE(check("0.5").empty());
    auto low = check("0.4");
    REQUIRE(low.size() == 1);
    REQUIRE(low[0].errorMessage == "Number is less than the minimum allowed value (min 0.5)");
    auto high = check("100");
    REQUIRE(high.size() == 1);
    REQUIRE(high[0].errorMessage == "Number is greater than the maximum allowed value (max 99.99)");

    SECTION("unset bounds do not constrain") {
        field.validators[0].minNumber.reset();
        field.validators[0].maxNumber.reset();
        REQUIRE(check("-1e9").empty());
    }
}

TEST_CASE("int validator rejects a fractional number and names its limit", "[validator][e1]") {
    RequestValidator validator;
    RequestFieldConfig field;
    field.fieldName = "qty";
    ValidatorConfig v;
    v.type = "int";
    v.min = 1;
    v.max = 1000;
    field.validators.push_back(v);
    std::map<std::string, std::string> params{{"qty", "301.7"}};
    auto errors = validator.validateRequestParameters({field}, params);
    REQUIRE(errors.size() == 1);
    REQUIRE(errors[0].errorMessage == "Invalid integer value");

    params["qty"] = "1001";
    errors = validator.validateRequestParameters({field}, params);
    REQUIRE(errors.size() == 1);
    REQUIRE(errors[0].errorMessage == "Integer is greater than the maximum allowed value (max 1000)");
}

TEST_CASE("request-fields-validation allows the format selector", "[validator][e1]") {
    RequestValidator validator;
    std::map<std::string, std::string> params{{"format", "csv"}, {"limit", "1"}, {"offset", "0"}};
    REQUIRE(validator.validateRequestFields({}, params).empty());
    params["nope"] = "1";
    REQUIRE(validator.validateRequestFields({}, params).size() == 1);
}

TEST_CASE("MCP schema emits number bounds only when set", "[mcp][schema][e1]") {
    RequestFieldConfig field;
    field.fieldName = "ratio";
    ValidatorConfig v;
    v.type = "number";
    field.validators.push_back(v);

    auto unset = crow::json::load(MCPSchemaBuilder::buildInputSchema({field}).dump());
    REQUIRE(unset["properties"]["ratio"]["type"].s() == "number");
    REQUIRE_FALSE(unset["properties"]["ratio"].has("minimum"));
    REQUIRE_FALSE(unset["properties"]["ratio"].has("maximum"));

    field.validators[0].minNumber = 0.0;
    field.validators[0].maxNumber = 2.5;
    auto set = crow::json::load(MCPSchemaBuilder::buildInputSchema({field}).dump());
    REQUIRE(set["properties"]["ratio"]["minimum"].d() == 0.0);
    REQUIRE(set["properties"]["ratio"]["maximum"].d() == 2.5);
}

TEST_CASE("QueryResult::formatDouble is the shortest round-trip form", "[query_executor][e1]") {
    REQUIRE(QueryResult::formatDouble(711.56) == "711.56");
    REQUIRE(QueryResult::formatDouble(0.1) == "0.1");
    REQUIRE(QueryResult::formatDouble(5.0) == "5.0");
    REQUIRE(QueryResult::formatDouble(1e300) == "1e+300");
    REQUIRE(QueryResult::formatDouble(-2.5e-8) == "-2.5e-08");
    REQUIRE(QueryResult::normaliseNumberText("711.55999999999994543") == "711.56");
    REQUIRE(QueryResult::normaliseNumberText("42") == "42");
}

TEST_CASE("Query results keep SELECT column order and short doubles", "[query_executor][e1]") {
    duckdb_database database;
    REQUIRE(duckdb_open(NULL, &database) == DuckDBSuccess);
    {
        QueryExecutor executor(database);
        executor.execute(
            "SELECT 'z' AS zeta, 711.56::DOUBLE AS price, 1 AS alpha, 0.1::FLOAT AS f, "
            "{'b': 1.25::DOUBLE} AS nested");
        QueryResult result;
        result.data = executor.toJson();
        result.columns = executor.columnNames();
        const std::string text = result.dumpData();
        INFO(text);
        REQUIRE(text == R"([{"zeta":"z","price":711.56,"alpha":1,"f":0.1,"nested":{"b":1.25}}])");
        REQUIRE(crow::json::load(text));
    }
    duckdb_close(&database);
}

} // namespace test
} // namespace flapi
