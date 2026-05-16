#include <catch2/catch_test_macros.hpp>

#include "config_manager.hpp"
#include "sql_parameter_classifier.hpp"

namespace flapi {
namespace test {

namespace {

RequestFieldConfig fieldWith(const std::string& name, const std::string& validator_type) {
    RequestFieldConfig f;
    f.fieldName = name;
    f.fieldIn = "query";
    if (!validator_type.empty()) {
        ValidatorConfig v;
        v.type = validator_type;
        f.validators.push_back(v);
    }
    return f;
}

RequestFieldConfig bareField(const std::string& name) {
    RequestFieldConfig f;
    f.fieldName = name;
    f.fieldIn = "query";
    return f;
}

} // namespace

TEST_CASE("SqlParameterClassifier: field with no validators is not bindable",
          "[security][prepared][classifier]") {
    // Without a validator we have no shape information for the value.
    // The conservative call is to leave it on the Mustache path; an
    // operator can opt in to binding by attaching a typed validator.
    SqlParameterClassifier c;
    auto r = c.classify(bareField("x"));
    REQUIRE_FALSE(r.bindable);
}

TEST_CASE("SqlParameterClassifier: empty validator type is not bindable",
          "[security][prepared][classifier]") {
    SqlParameterClassifier c;
    RequestFieldConfig f = bareField("x");
    f.validators.push_back(ValidatorConfig{});  // type == ""
    auto r = c.classify(f);
    REQUIRE_FALSE(r.bindable);
}

TEST_CASE("SqlParameterClassifier: int validator binds as Integer",
          "[security][prepared][classifier]") {
    SqlParameterClassifier c;
    auto r = c.classify(fieldWith("id", "int"));
    REQUIRE(r.bindable);
    REQUIRE(r.type == SqlParameterType::Integer);
}

TEST_CASE("SqlParameterClassifier: integer alias also maps to Integer",
          "[security][prepared][classifier]") {
    SqlParameterClassifier c;
    auto r = c.classify(fieldWith("id", "integer"));
    REQUIRE(r.bindable);
    REQUIRE(r.type == SqlParameterType::Integer);
}

TEST_CASE("SqlParameterClassifier: number validator binds as Double",
          "[security][prepared][classifier]") {
    SqlParameterClassifier c;
    auto r = c.classify(fieldWith("amount", "number"));
    REQUIRE(r.bindable);
    REQUIRE(r.type == SqlParameterType::Double);
}

TEST_CASE("SqlParameterClassifier: float / double aliases map to Double",
          "[security][prepared][classifier]") {
    SqlParameterClassifier c;
    REQUIRE(c.classify(fieldWith("a", "float")).type == SqlParameterType::Double);
    REQUIRE(c.classify(fieldWith("b", "double")).type == SqlParameterType::Double);
}

TEST_CASE("SqlParameterClassifier: boolean validator binds as Boolean",
          "[security][prepared][classifier]") {
    SqlParameterClassifier c;
    auto r = c.classify(fieldWith("active", "boolean"));
    REQUIRE(r.bindable);
    REQUIRE(r.type == SqlParameterType::Boolean);
}

TEST_CASE("SqlParameterClassifier: date validator binds as Date",
          "[security][prepared][classifier]") {
    SqlParameterClassifier c;
    auto r = c.classify(fieldWith("created", "date"));
    REQUIRE(r.bindable);
    REQUIRE(r.type == SqlParameterType::Date);
}

TEST_CASE("SqlParameterClassifier: time validator binds as Time",
          "[security][prepared][classifier]") {
    SqlParameterClassifier c;
    auto r = c.classify(fieldWith("at", "time"));
    REQUIRE(r.bindable);
    REQUIRE(r.type == SqlParameterType::Time);
}

TEST_CASE("SqlParameterClassifier: uuid validator binds as Varchar",
          "[security][prepared][classifier]") {
    // DuckDB will parse the UUID from a VARCHAR binding; treating it as
    // Varchar keeps the binder simple and correct.
    SqlParameterClassifier c;
    auto r = c.classify(fieldWith("id", "uuid"));
    REQUIRE(r.bindable);
    REQUIRE(r.type == SqlParameterType::Varchar);
}

TEST_CASE("SqlParameterClassifier: string validator binds as Varchar",
          "[security][prepared][classifier]") {
    SqlParameterClassifier c;
    auto r = c.classify(fieldWith("name", "string"));
    REQUIRE(r.bindable);
    REQUIRE(r.type == SqlParameterType::Varchar);
}

TEST_CASE("SqlParameterClassifier: email / enum validators bind as Varchar",
          "[security][prepared][classifier]") {
    SqlParameterClassifier c;
    REQUIRE(c.classify(fieldWith("e", "email")).type == SqlParameterType::Varchar);
    REQUIRE(c.classify(fieldWith("s", "enum")).type == SqlParameterType::Varchar);
}

TEST_CASE("SqlParameterClassifier: unknown validator type is not bindable",
          "[security][prepared][classifier]") {
    // Forward-compat: a future validator we don't know about must NOT
    // accidentally land on the prepared path. Falling back to Mustache
    // is the safe default.
    SqlParameterClassifier c;
    auto r = c.classify(fieldWith("x", "future-type-from-2030"));
    REQUIRE_FALSE(r.bindable);
}

TEST_CASE("SqlParameterClassifier: multiple validators take the first known type",
          "[security][prepared][classifier]") {
    // A field can carry several validators (range AND pattern, say).
    // We pick the first one that resolves to a typed binding so behaviour
    // is deterministic.
    SqlParameterClassifier c;
    RequestFieldConfig f = bareField("x");
    ValidatorConfig v1; v1.type = "int";
    ValidatorConfig v2; v2.type = "string";
    f.validators.push_back(v1);
    f.validators.push_back(v2);
    auto r = c.classify(f);
    REQUIRE(r.bindable);
    REQUIRE(r.type == SqlParameterType::Integer);
}

TEST_CASE("SqlParameterClassifier: multiple validators, unknown first then known, picks the known",
          "[security][prepared][classifier][edge]") {
    // Forward-compat: a validator we don't recognise must NOT block a
    // later recognised one from making the field bindable.
    SqlParameterClassifier c;
    RequestFieldConfig f = bareField("x");
    ValidatorConfig v1; v1.type = "future-type-from-2030";
    ValidatorConfig v2; v2.type = "int";
    f.validators.push_back(v1);
    f.validators.push_back(v2);
    auto r = c.classify(f);
    REQUIRE(r.bindable);
    REQUIRE(r.type == SqlParameterType::Integer);
}

TEST_CASE("SqlParameterClassifier: validator type with stray whitespace is not bindable",
          "[security][prepared][classifier][edge]") {
    // YAML doesn't trim values by default; an operator who writes
    // `type: " int"` must not silently land on the prepared path.
    SqlParameterClassifier c;
    REQUIRE_FALSE(c.classify(fieldWith("x", " int")).bindable);
    REQUIRE_FALSE(c.classify(fieldWith("x", "int ")).bindable);
}

TEST_CASE("SqlParameterClassifier: empty field with empty validators is not bindable",
          "[security][prepared][classifier][edge]") {
    SqlParameterClassifier c;
    RequestFieldConfig empty;
    REQUIRE_FALSE(c.classify(empty).bindable);
}

TEST_CASE("SqlParameterClassifier: very large validator list still picks the first known",
          "[security][prepared][classifier][edge]") {
    // Bookkeeping smoke test — make sure iteration ordering is stable
    // and doesn't depend on validator count.
    SqlParameterClassifier c;
    RequestFieldConfig f = bareField("x");
    for (int i = 0; i < 50; ++i) {
        ValidatorConfig v; v.type = "unknown-" + std::to_string(i);
        f.validators.push_back(v);
    }
    ValidatorConfig known; known.type = "boolean";
    f.validators.push_back(known);
    auto r = c.classify(f);
    REQUIRE(r.bindable);
    REQUIRE(r.type == SqlParameterType::Boolean);
}

TEST_CASE("SqlParameterClassifier: bindability decision is deterministic across calls",
          "[security][prepared][classifier][edge]") {
    SqlParameterClassifier c;
    auto f = fieldWith("x", "int");
    auto a = c.classify(f);
    auto b = c.classify(f);
    auto cc = c.classify(f);
    REQUIRE(a.bindable == b.bindable);
    REQUIRE(b.bindable == cc.bindable);
    REQUIRE(a.type == b.type);
    REQUIRE(b.type == cc.type);
}

TEST_CASE("SqlParameterClassifier: validator type comparison is case-sensitive",
          "[security][prepared][classifier]") {
    // YAML config conventions in flapi are all lowercase; tolerating
    // case differences would let `Int` quietly bypass the typed path on
    // some platforms. Explicit type names only.
    SqlParameterClassifier c;
    REQUIRE_FALSE(c.classify(fieldWith("x", "Int")).bindable);
    REQUIRE_FALSE(c.classify(fieldWith("x", "STRING")).bindable);
}

} // namespace test
} // namespace flapi
