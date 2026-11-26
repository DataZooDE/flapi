#include <catch2/catch_test_macros.hpp>

#include "config_manager.hpp"
#include "prepared_template_rewriter.hpp"

namespace flapi {
namespace test {

namespace {

RequestFieldConfig typedField(const std::string& name, const std::string& validator_type) {
    RequestFieldConfig f;
    f.fieldName = name;
    f.fieldIn = "query";
    ValidatorConfig v;
    v.type = validator_type;
    f.validators.push_back(v);
    return f;
}

RequestFieldConfig bareField(const std::string& name) {
    RequestFieldConfig f;
    f.fieldName = name;
    f.fieldIn = "query";
    return f;
}

} // namespace

TEST_CASE("PreparedTemplateRewriter: empty template yields empty result with no bindings",
          "[security][prepared][rewriter]") {
    PreparedTemplateRewriter rewriter;
    SqlParameterClassifier classifier;
    auto r = rewriter.rewrite("", {}, classifier);
    REQUIRE(r.rewritten_template.empty());
    REQUIRE(r.bindings.empty());
}

TEST_CASE("PreparedTemplateRewriter: template with no params is left alone",
          "[security][prepared][rewriter]") {
    PreparedTemplateRewriter rewriter;
    SqlParameterClassifier classifier;
    auto r = rewriter.rewrite("SELECT 1", {}, classifier);
    REQUIRE(r.rewritten_template == "SELECT 1");
    REQUIRE(r.bindings.empty());
}

TEST_CASE("PreparedTemplateRewriter: simple {{ params.X }} with int field is rewritten to ?",
          "[security][prepared][rewriter]") {
    PreparedTemplateRewriter rewriter;
    SqlParameterClassifier classifier;
    auto r = rewriter.rewrite(
        "SELECT * FROM t WHERE id = {{ params.id }}",
        {typedField("id", "int")},
        classifier);

    REQUIRE(r.rewritten_template == "SELECT * FROM t WHERE id = ?");
    REQUIRE(r.bindings.size() == 1);
    REQUIRE(r.bindings[0].field_name == "id");
    REQUIRE(r.bindings[0].type == SqlParameterType::Integer);
    REQUIRE(r.bindings[0].position == 0);
}

TEST_CASE("PreparedTemplateRewriter: a quoted triple-brace site binds and drops its quotes (#192)",
          "[security][prepared][rewriter]") {
    PreparedTemplateRewriter rewriter;
    SqlParameterClassifier classifier;
    auto r = rewriter.rewrite(
        "SELECT * FROM t WHERE n = '{{{ params.name }}}'",
        {typedField("name", "string")},
        classifier);

    REQUIRE(r.rewritten_template == "SELECT * FROM t WHERE n = ?");
    REQUIRE(r.bindings.size() == 1);
    REQUIRE(r.bindings[0].field_name == "name");
    REQUIRE(r.bindings[0].type == SqlParameterType::Varchar);
    REQUIRE(r.bindings[0].absent_as_empty);
}

TEST_CASE("PreparedTemplateRewriter: an unquoted triple-brace site binds with the field's type (#192)",
          "[security][prepared][rewriter]") {
    PreparedTemplateRewriter rewriter;
    SqlParameterClassifier classifier;
    auto r = rewriter.rewrite("SELECT * FROM t WHERE id = {{{ params.id }}} LIMIT {{{ params.n }}}",
                              {typedField("id", "int"), typedField("n", "int")}, classifier);
    REQUIRE(r.rewritten_template == "SELECT * FROM t WHERE id = ? LIMIT ?");
    REQUIRE(r.bindings.size() == 2);
    REQUIRE(r.bindings[0].type == SqlParameterType::Integer);
    REQUIRE_FALSE(r.bindings[0].absent_as_empty);
}

TEST_CASE("PreparedTemplateRewriter: a LIKE pattern becomes a concatenation (#192)",
          "[security][prepared][rewriter]") {
    PreparedTemplateRewriter rewriter;
    SqlParameterClassifier classifier;
    auto r = rewriter.rewrite("WHERE a LIKE '%{{{ params.x }}}%' AND b LIKE '{{{ params.x }}}_' AND c = 1",
                              {typedField("x", "string")}, classifier);
    REQUIRE(r.rewritten_template == "WHERE a LIKE ('%' || ? || '%') AND b LIKE (? || '_') AND c = 1");
    REQUIRE(r.bindings.size() == 2);
    REQUIRE(r.bindings[0].position == 0);
    REQUIRE(r.bindings[1].position == 1);
}

TEST_CASE("PreparedTemplateRewriter: triple-brace sites that are not plain values stay literal (#192)",
          "[security][prepared][rewriter]") {
    PreparedTemplateRewriter rewriter;
    SqlParameterClassifier classifier;
    const std::vector<RequestFieldConfig> fields = {typedField("x", "string"), typedField("d", "date"),
                                                    typedField("k", "enum")};
    const std::vector<std::string> kept = {
        "SELECT DATE '{{{ params.d }}}'",                    // typed literal
        "SELECT INTERVAL '{{{ params.x }}}' DAY",            // typed literal
        "ATTACH '{{{ params.x }}}' AS a",                    // statement argument
        "SELECT * FROM '{{{ params.x }}}'",                  // replacement scan
        "SELECT * FROM {{{ params.x }}}",                    // a name
        "SELECT * FROM t ORDER BY {{{ params.k }}}",         // whitelisted identifier
        "SELECT * FROM t_{{{ params.x }}}",                  // part of a name
        "SELECT * FROM t AT (VERSION => {{{ params.x }}})",  // DuckLake resolves AT before binding
        "SELECT * FROM t AT (TIMESTAMP => CAST('{{{ params.x }}}' AS TIMESTAMP))",
        "SELECT * FROM t {{#params.x}}WHERE n = '{{{ params.x }}}'{{/params.x}}",   // inside a section
    };
    for (const auto& text : kept) {
        INFO(text);
        auto r = rewriter.rewrite(text, fields, classifier);
        REQUIRE(r.rewritten_template.find("{{{ params.") != std::string::npos);
    }
}

TEST_CASE("PreparedTemplateRewriter: a quoted double-brace site binds as a whole literal (#277)",
          "[security][prepared][rewriter]") {
    PreparedTemplateRewriter rewriter;
    SqlParameterClassifier classifier;
    auto r = rewriter.rewrite("SELECT * FROM t WHERE n = '{{ params.name }}'",
                              {typedField("name", "string")}, classifier);
    REQUIRE(r.rewritten_template == "SELECT * FROM t WHERE n = ?");
    REQUIRE(r.bindings.size() == 1);
    REQUIRE(r.bindings[0].field_name == "name");
    REQUIRE(r.bindings[0].type == SqlParameterType::Varchar);
    REQUIRE(r.bindings[0].absent_as_empty);
}

TEST_CASE("PreparedTemplateRewriter: a double-brace LIKE pattern becomes a concatenation (#277)",
          "[security][prepared][rewriter]") {
    PreparedTemplateRewriter rewriter;
    SqlParameterClassifier classifier;
    auto r = rewriter.rewrite("WHERE a LIKE '%{{ params.x }}%' AND b LIKE '{{params.x}}_' AND c = 1",
                              {typedField("x", "string")}, classifier);
    REQUIRE(r.rewritten_template == "WHERE a LIKE ('%' || ? || '%') AND b LIKE (? || '_') AND c = 1");
    REQUIRE(r.bindings.size() == 2);
    REQUIRE(r.bindings[1].position == 1);
}

TEST_CASE("PreparedTemplateRewriter: a literal mixing double and triple braces binds as a whole (#277)",
          "[security][prepared][rewriter]") {
    PreparedTemplateRewriter rewriter;
    SqlParameterClassifier classifier;
    auto r = rewriter.rewrite("SELECT '{{ params.x }} and {{{ params.x }}}'", {typedField("x", "string")},
                              classifier);
    REQUIRE(r.rewritten_template == "SELECT (? || ' and ' || ?)");
    REQUIRE(r.bindings.size() == 2);
}

TEST_CASE("PreparedTemplateRewriter: double-brace sites that are not plain values stay literal (#277)",
          "[security][prepared][rewriter]") {
    PreparedTemplateRewriter rewriter;
    SqlParameterClassifier classifier;
    const std::vector<RequestFieldConfig> fields = {typedField("x", "string"), typedField("d", "date"),
                                                    typedField("k", "enum")};
    const std::vector<std::string> kept = {
        "SELECT DATE '{{ params.d }}'",                      // typed literal
        "SELECT INTERVAL '{{ params.x }}' DAY",              // typed literal
        "ATTACH '{{ params.x }}' AS a",                      // statement argument
        "SELECT * FROM '{{ params.x }}'",                    // replacement scan
        "SELECT * FROM {{ params.x }}",                      // a name
        "SELECT * FROM t ORDER BY {{ params.k }}",           // whitelisted identifier
        "SELECT * FROM t ORDER BY a, {{ params.k }}",        // an enum in a list is a name too
        "SELECT * FROM t ORDER BY a {{ params.k }}",         // e.g. ASC / DESC
        "SELECT * FROM t_{{ params.x }}",                    // part of a name
        "SELECT * FROM t AT (TIMESTAMP => CAST('{{ params.x }}' AS TIMESTAMP))",   // a string in AT
        "SELECT * FROM t {{#params.x}}WHERE n = '{{ params.x }}'{{/params.x}}",   // inside a section
        "SELECT 1 -- n = {{ params.x }}",                    // a comment
    };
    for (const auto& text : kept) {
        INFO(text);
        auto r = rewriter.rewrite(text, fields, classifier);
        REQUIRE(r.rewritten_template == text);
        REQUIRE(r.bindings.empty());
    }
}

TEST_CASE("PreparedTemplateRewriter: an unquoted double brace in AT (...) stays bound (#277)",
          "[security][prepared][rewriter]") {
    // DuckLake takes a bound int or date there (a VERSION, a day); rendered as
    // text, an unquoted 2024-01-02 would be integer arithmetic.
    PreparedTemplateRewriter rewriter;
    SqlParameterClassifier classifier;
    auto r = rewriter.rewrite("SELECT * FROM t AT (VERSION => {{ params.v }})", {typedField("v", "int")},
                              classifier);
    REQUIRE(r.rewritten_template == "SELECT * FROM t AT (VERSION => ?)");
    REQUIRE(r.bindings.size() == 1);
}

TEST_CASE("PreparedTemplateRewriter: an enum after a comparison is still a bound value (#277)",
          "[security][prepared][rewriter]") {
    PreparedTemplateRewriter rewriter;
    SqlParameterClassifier classifier;
    auto r = rewriter.rewrite("SELECT * FROM t WHERE status = {{ params.k }} OR s IN ({{ params.k }})",
                              {typedField("k", "enum")}, classifier);
    REQUIRE(r.rewritten_template == "SELECT * FROM t WHERE status = ? OR s IN (?)");
    REQUIRE(r.bindings.size() == 2);
    REQUIRE(r.bindings[0].type == SqlParameterType::Varchar);
}

TEST_CASE("PreparedTemplateRewriter: a double-brace literal with an undeclared or untyped param is left alone (#277)",
          "[security][prepared][rewriter]") {
    PreparedTemplateRewriter rewriter;
    SqlParameterClassifier classifier;
    const std::string in = "WHERE n = '{{ params.free }}' AND m = '{{ params.nope }}' AND p = '{{ conn.x }}'";
    auto r = rewriter.rewrite(in, {bareField("free")}, classifier);
    REQUIRE(r.rewritten_template == in);
    REQUIRE(r.bindings.empty());
}

TEST_CASE("PreparedTemplateRewriter: a literal with an undeclared or untyped param is left alone (#192)",
          "[security][prepared][rewriter]") {
    PreparedTemplateRewriter rewriter;
    SqlParameterClassifier classifier;
    auto r = rewriter.rewrite("WHERE n = '{{{ params.free }}}' AND m = '{{{ params.nope }}}'",
                              {bareField("free")}, classifier);
    REQUIRE(r.rewritten_template == "WHERE n = '{{{ params.free }}}' AND m = '{{{ params.nope }}}'");
    REQUIRE(r.bindings.empty());
}

TEST_CASE("PreparedTemplateRewriter: escaped quotes and comments do not confuse the literal scan (#192)",
          "[security][prepared][rewriter]") {
    PreparedTemplateRewriter rewriter;
    SqlParameterClassifier classifier;
    auto r = rewriter.rewrite("-- it's a comment with '{{{ params.x }}}'\n"
                              "SELECT 'it''s', n FROM t WHERE n = '{{{ params.x }}}'",
                              {typedField("x", "string")}, classifier);
    REQUIRE(r.rewritten_template ==
            "-- it's a comment with '{{{ params.x }}}'\nSELECT 'it''s', n FROM t WHERE n = ?");
    REQUIRE(r.bindings.size() == 1);
}

TEST_CASE("PreparedTemplateRewriter: param with no validator is left on the Mustache path",
          "[security][prepared][rewriter]") {
    PreparedTemplateRewriter rewriter;
    SqlParameterClassifier classifier;
    auto r = rewriter.rewrite(
        "SELECT * FROM t WHERE id = {{ params.id }}",
        {bareField("id")},
        classifier);

    REQUIRE(r.rewritten_template == "SELECT * FROM t WHERE id = {{ params.id }}");
    REQUIRE(r.bindings.empty());
}

TEST_CASE("PreparedTemplateRewriter: param missing from request_fields is left alone",
          "[security][prepared][rewriter]") {
    // A template that references an undeclared parameter is a config
    // error, but the rewriter must not crash or invent bindings —
    // existing flapi validation handles the diagnostic separately.
    PreparedTemplateRewriter rewriter;
    SqlParameterClassifier classifier;
    auto r = rewriter.rewrite(
        "SELECT {{ params.mystery }}",
        {typedField("known", "int")},
        classifier);

    REQUIRE(r.rewritten_template == "SELECT {{ params.mystery }}");
    REQUIRE(r.bindings.empty());
}

TEST_CASE("PreparedTemplateRewriter: param inside {{#section}}...{{/section}} is left alone",
          "[security][prepared][rewriter]") {
    // A bindable param inside a Mustache conditional section is risky:
    // if the section evaluates falsy at render time the `?` would
    // disappear but the binding would still be queued, breaking position
    // counts. v1 keeps the entire section on the Mustache path.
    PreparedTemplateRewriter rewriter;
    SqlParameterClassifier classifier;
    const std::string in =
        "SELECT * FROM t WHERE 1=1 "
        "{{#params.id}}AND id = {{ params.id }}{{/params.id}}";
    auto r = rewriter.rewrite(in, {typedField("id", "int")}, classifier);

    REQUIRE(r.rewritten_template == in);
    REQUIRE(r.bindings.empty());
}

TEST_CASE("PreparedTemplateRewriter: param after a closed section is rewritten",
          "[security][prepared][rewriter]") {
    // Section depth must return to 0 after the matching closing tag,
    // so subsequent top-level params are eligible again.
    PreparedTemplateRewriter rewriter;
    SqlParameterClassifier classifier;
    const std::string in =
        "WITH cte AS ({{#params.flag}}SELECT 1{{/params.flag}}) "
        "SELECT id FROM t WHERE id = {{ params.id }}";
    auto r = rewriter.rewrite(in, {typedField("id", "int"), typedField("flag", "int")}, classifier);

    REQUIRE(r.rewritten_template.find("?") != std::string::npos);
    REQUIRE(r.bindings.size() == 1);
    REQUIRE(r.bindings[0].field_name == "id");
}

TEST_CASE("PreparedTemplateRewriter: multiple top-level params produce ordered bindings",
          "[security][prepared][rewriter]") {
    PreparedTemplateRewriter rewriter;
    SqlParameterClassifier classifier;
    auto r = rewriter.rewrite(
        "SELECT * FROM t WHERE id = {{ params.id }} AND status = {{ params.status }}",
        {typedField("id", "int"), typedField("status", "string")},
        classifier);

    REQUIRE(r.bindings.size() == 2);
    REQUIRE(r.bindings[0].field_name == "id");
    REQUIRE(r.bindings[0].position == 0);
    REQUIRE(r.bindings[0].type == SqlParameterType::Integer);
    REQUIRE(r.bindings[1].field_name == "status");
    REQUIRE(r.bindings[1].position == 1);
    REQUIRE(r.bindings[1].type == SqlParameterType::Varchar);
}

TEST_CASE("PreparedTemplateRewriter: repeated occurrence of a single param produces two bindings",
          "[security][prepared][rewriter]") {
    // DuckDB's prepared API uses positional `?` — repeating the same
    // logical param twice means two physical bindings, not one.
    PreparedTemplateRewriter rewriter;
    SqlParameterClassifier classifier;
    auto r = rewriter.rewrite(
        "SELECT {{ params.id }} AS a, {{ params.id }} AS b",
        {typedField("id", "int")},
        classifier);

    REQUIRE(r.bindings.size() == 2);
    REQUIRE(r.bindings[0].field_name == "id");
    REQUIRE(r.bindings[1].field_name == "id");
    REQUIRE(r.bindings[0].position == 0);
    REQUIRE(r.bindings[1].position == 1);
}

TEST_CASE("PreparedTemplateRewriter: whitespace inside braces is tolerated",
          "[security][prepared][rewriter]") {
    PreparedTemplateRewriter rewriter;
    SqlParameterClassifier classifier;
    auto r = rewriter.rewrite(
        "SELECT {{  params.id  }} FROM t",
        {typedField("id", "int")},
        classifier);
    REQUIRE(r.bindings.size() == 1);
}

TEST_CASE("PreparedTemplateRewriter: inverted section ({{^X}}) also pauses rewriting",
          "[security][prepared][rewriter]") {
    // Mustache inverted sections (`{{^X}}...{{/X}}`) have the same
    // depth semantics as positive ones; bindable params inside them
    // must NOT be rewritten.
    PreparedTemplateRewriter rewriter;
    SqlParameterClassifier classifier;
    const std::string in =
        "SELECT * FROM t {{^params.skip}}WHERE id = {{ params.id }}{{/params.skip}}";
    auto r = rewriter.rewrite(in, {typedField("id", "int"), typedField("skip", "int")}, classifier);

    REQUIRE(r.rewritten_template == in);
    REQUIRE(r.bindings.empty());
}

TEST_CASE("PreparedTemplateRewriter: unterminated double-brace tag is left alone",
          "[security][prepared][rewriter][edge]") {
    // Defensive: a malformed template must never crash or produce an
    // invalid binding plan. The whole tail of the template flows
    // through as untouched text.
    PreparedTemplateRewriter rewriter;
    SqlParameterClassifier classifier;
    auto r = rewriter.rewrite(
        "SELECT * FROM t WHERE id = {{ params.id no closing brace",
        {typedField("id", "int")},
        classifier);
    REQUIRE(r.bindings.empty());
    REQUIRE(r.rewritten_template.find("{{ params.id") != std::string::npos);
}

TEST_CASE("PreparedTemplateRewriter: unterminated triple-brace is left alone",
          "[security][prepared][rewriter][edge]") {
    PreparedTemplateRewriter rewriter;
    SqlParameterClassifier classifier;
    auto r = rewriter.rewrite(
        "SELECT '{{{ params.name and then nothing",
        {typedField("name", "string")},
        classifier);
    REQUIRE(r.bindings.empty());
}

TEST_CASE("PreparedTemplateRewriter: newlines and tabs inside tags are tolerated",
          "[security][prepared][rewriter][edge]") {
    PreparedTemplateRewriter rewriter;
    SqlParameterClassifier classifier;
    auto r = rewriter.rewrite(
        "SELECT * FROM t WHERE id = {{\tparams.id\t}}",
        {typedField("id", "int")},
        classifier);
    REQUIRE(r.bindings.size() == 1);
    REQUIRE(r.rewritten_template.find('?') != std::string::npos);
}

TEST_CASE("PreparedTemplateRewriter: no-whitespace form {{params.X}} also matches",
          "[security][prepared][rewriter][edge]") {
    PreparedTemplateRewriter rewriter;
    SqlParameterClassifier classifier;
    auto r = rewriter.rewrite("WHERE id = {{params.id}}",
                              {typedField("id", "int")}, classifier);
    REQUIRE(r.bindings.size() == 1);
    REQUIRE(r.rewritten_template == "WHERE id = ?");
}

TEST_CASE("PreparedTemplateRewriter: nested sections still suppress rewriting",
          "[security][prepared][rewriter][edge]") {
    // Depth must increment for every opening tag and decrement for
    // every close, so a bindable param nested two sections deep is
    // still left for Mustache.
    PreparedTemplateRewriter rewriter;
    SqlParameterClassifier classifier;
    const std::string in =
        "{{#params.a}}{{#params.b}}WHERE id = {{ params.id }}{{/params.b}}{{/params.a}}";
    auto r = rewriter.rewrite(in,
                              {typedField("a", "int"), typedField("b", "int"),
                               typedField("id", "int")},
                              classifier);
    REQUIRE(r.bindings.empty());
    REQUIRE(r.rewritten_template == in);
}

TEST_CASE("PreparedTemplateRewriter: stray closing section without an open is harmless",
          "[security][prepared][rewriter][edge]") {
    // Bug-bait input. The depth counter clamps at zero so a stray
    // `{{/X}}` doesn't underflow, and subsequent params remain bindable.
    PreparedTemplateRewriter rewriter;
    SqlParameterClassifier classifier;
    auto r = rewriter.rewrite("{{/orphan}} SELECT {{ params.id }} FROM t",
                              {typedField("id", "int")}, classifier);
    REQUIRE(r.bindings.size() == 1);
}

TEST_CASE("PreparedTemplateRewriter: section open without matching close keeps depth nonzero",
          "[security][prepared][rewriter][edge]") {
    // If the operator forgets to close a section, every subsequent
    // bindable param stays on the Mustache path. This is a strictly
    // safer-than-alternative behaviour: better an unbound param that
    // Mustache renders than a bound `?` whose surrounding section
    // disappears at render time.
    PreparedTemplateRewriter rewriter;
    SqlParameterClassifier classifier;
    auto r = rewriter.rewrite(
        "{{#params.a}}WHERE id = {{ params.id }} no-close-tag",
        {typedField("a", "int"), typedField("id", "int")},
        classifier);
    REQUIRE(r.bindings.empty());
}

TEST_CASE("PreparedTemplateRewriter: empty params.name (just 'params.') is left alone",
          "[security][prepared][rewriter][edge]") {
    PreparedTemplateRewriter rewriter;
    SqlParameterClassifier classifier;
    auto r = rewriter.rewrite("SELECT {{ params. }}",
                              {typedField("id", "int")}, classifier);
    REQUIRE(r.bindings.empty());
}

TEST_CASE("PreparedTemplateRewriter: bare {{ params }} without a dot is left alone",
          "[security][prepared][rewriter][edge]") {
    PreparedTemplateRewriter rewriter;
    SqlParameterClassifier classifier;
    auto r = rewriter.rewrite("SELECT {{ params }}",
                              {typedField("id", "int")}, classifier);
    REQUIRE(r.bindings.empty());
}

TEST_CASE("PreparedTemplateRewriter: multi-line template binds across line boundaries",
          "[security][prepared][rewriter][edge]") {
    PreparedTemplateRewriter rewriter;
    SqlParameterClassifier classifier;
    const std::string in =
        "SELECT *\n"
        "FROM customers\n"
        "WHERE id = {{ params.id }}\n"
        "  AND region = {{ params.region }}\n"
        "ORDER BY id";
    auto r = rewriter.rewrite(in,
                              {typedField("id", "int"), typedField("region", "string")},
                              classifier);
    REQUIRE(r.bindings.size() == 2);
    REQUIRE(r.bindings[0].field_name == "id");
    REQUIRE(r.bindings[1].field_name == "region");
}

TEST_CASE("PreparedTemplateRewriter: many distinct bindings preserve order strictly",
          "[security][prepared][rewriter][edge]") {
    // Smoke-test the binding-position bookkeeping against a non-trivial
    // count. DuckDB binds by position; an off-by-one would corrupt every
    // query in production.
    PreparedTemplateRewriter rewriter;
    SqlParameterClassifier classifier;
    std::vector<RequestFieldConfig> fields;
    std::string in = "SELECT 1";
    for (int i = 0; i < 25; ++i) {
        const std::string name = "p" + std::to_string(i);
        fields.push_back(typedField(name, "int"));
        in += " AND col = {{ params." + name + " }}";
    }
    auto r = rewriter.rewrite(in, fields, classifier);
    REQUIRE(r.bindings.size() == 25);
    for (std::size_t i = 0; i < r.bindings.size(); ++i) {
        REQUIRE(r.bindings[i].field_name == "p" + std::to_string(i));
        REQUIRE(r.bindings[i].position == i);
    }
}

TEST_CASE("PreparedTemplateRewriter: classic SQL-injection payload as a value is bound, not injected",
          "[security][prepared][rewriter][edge]") {
    // The rewriter itself doesn't see values — only the template. But
    // the proof of the security guarantee is: the template references
    // `params.id` once and produces one `?` and one binding. The
    // VALUE of params.id at runtime (whatever it is, including a
    // classic injection payload) lands in DuckDB's bind buffer, never
    // as syntax. This test pins the binding count so any future change
    // that "expands" the value into the template would fail loudly.
    PreparedTemplateRewriter rewriter;
    SqlParameterClassifier classifier;
    auto r = rewriter.rewrite(
        "SELECT * FROM users WHERE id = {{ params.id }}",
        {typedField("id", "int")},
        classifier);
    REQUIRE(r.rewritten_template == "SELECT * FROM users WHERE id = ?");
    REQUIRE(r.bindings.size() == 1);
}

TEST_CASE("PreparedTemplateRewriter: rewriting is idempotent under repeated invocation",
          "[security][prepared][rewriter][edge]") {
    // Sanity: running the rewriter on its own output must be a no-op
    // (no `{{ params.X }}` left to rewrite the second time).
    PreparedTemplateRewriter rewriter;
    SqlParameterClassifier classifier;
    auto first = rewriter.rewrite("WHERE id = {{ params.id }}",
                                  {typedField("id", "int")}, classifier);
    auto second = rewriter.rewrite(first.rewritten_template,
                                   {typedField("id", "int")}, classifier);
    REQUIRE(second.rewritten_template == first.rewritten_template);
    REQUIRE(second.bindings.empty());
}

TEST_CASE("PreparedTemplateRewriter: param names not starting with 'params.' are ignored",
          "[security][prepared][rewriter]") {
    // The rewriter only owns `params.*` — `conn.*` / `env.*` / `auth.*`
    // are connection/env/auth variables that the existing Mustache path
    // populates with operator-controlled values, not user input.
    PreparedTemplateRewriter rewriter;
    SqlParameterClassifier classifier;
    auto r = rewriter.rewrite(
        "SELECT * FROM read_parquet('{{{ conn.path }}}') WHERE x = {{ conn.x }}",
        {typedField("x", "int")},  // x is a request field, but `conn.x` is not `params.x`
        classifier);

    REQUIRE(r.bindings.empty());
}

} // namespace test
} // namespace flapi
