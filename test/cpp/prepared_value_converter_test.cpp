#include <catch2/catch_test_macros.hpp>

#include "prepared_value_converter.hpp"

using namespace flapi;

namespace {

PreparedValue okValue(SqlParameterType type, const std::string& raw, bool present = true) {
    PreparedValueConverter c;
    auto outcome = c.convert(type, raw, present);
    REQUIRE(outcome.ok);
    return outcome.value;
}

} // namespace

TEST_CASE("PreparedValueConverter: absent param is Null regardless of type", "[prepared_value_converter]") {
    PreparedValueConverter c;
    for (auto t : {SqlParameterType::Integer, SqlParameterType::Double, SqlParameterType::Boolean,
                   SqlParameterType::Date, SqlParameterType::Time, SqlParameterType::Varchar}) {
        auto outcome = c.convert(t, "", false);
        REQUIRE(outcome.ok);
        REQUIRE(outcome.value.kind == PreparedValue::Kind::Null);
    }
}

TEST_CASE("PreparedValueConverter: Integer parses signed int64", "[prepared_value_converter]") {
    PreparedValueConverter c;

    SECTION("positive") {
        auto v = okValue(SqlParameterType::Integer, "42");
        REQUIRE(v.kind == PreparedValue::Kind::Int64);
        REQUIRE(v.int64_value == 42);
    }

    SECTION("negative") {
        auto v = okValue(SqlParameterType::Integer, "-7");
        REQUIRE(v.kind == PreparedValue::Kind::Int64);
        REQUIRE(v.int64_value == -7);
    }

    SECTION("zero") {
        auto v = okValue(SqlParameterType::Integer, "0");
        REQUIRE(v.int64_value == 0);
    }

    SECTION("int64 boundary - max") {
        auto v = okValue(SqlParameterType::Integer, "9223372036854775807");
        REQUIRE(v.int64_value == 9223372036854775807LL);
    }

    SECTION("int64 boundary - min") {
        auto v = okValue(SqlParameterType::Integer, "-9223372036854775808");
        REQUIRE(v.int64_value == INT64_MIN);
    }

    SECTION("leading whitespace tolerated") {
        // stoll accepts leading whitespace; we match that for forgiveness
        auto v = okValue(SqlParameterType::Integer, "  17");
        REQUIRE(v.int64_value == 17);
    }

    SECTION("trailing garbage is rejected") {
        auto outcome = c.convert(SqlParameterType::Integer, "17x", true);
        REQUIRE_FALSE(outcome.ok);
        REQUIRE(outcome.error.find("Integer") != std::string::npos);
    }

    SECTION("not a number") {
        auto outcome = c.convert(SqlParameterType::Integer, "abc", true);
        REQUIRE_FALSE(outcome.ok);
    }

    SECTION("empty string") {
        auto outcome = c.convert(SqlParameterType::Integer, "", true);
        REQUIRE_FALSE(outcome.ok);
    }

    SECTION("scientific notation rejected - integer must be exact") {
        auto outcome = c.convert(SqlParameterType::Integer, "1e3", true);
        REQUIRE_FALSE(outcome.ok);
    }

    SECTION("decimal point rejected") {
        auto outcome = c.convert(SqlParameterType::Integer, "1.5", true);
        REQUIRE_FALSE(outcome.ok);
    }

    SECTION("overflow surfaces as error, not silent truncation") {
        auto outcome = c.convert(SqlParameterType::Integer, "99999999999999999999", true);
        REQUIRE_FALSE(outcome.ok);
    }

    SECTION("classic SQL injection payload errors instead of being parsed") {
        auto outcome = c.convert(SqlParameterType::Integer, "1; DROP TABLE users--", true);
        REQUIRE_FALSE(outcome.ok);
    }
}

TEST_CASE("PreparedValueConverter: Double parses IEEE-754", "[prepared_value_converter]") {
    PreparedValueConverter c;

    SECTION("positive double") {
        auto v = okValue(SqlParameterType::Double, "3.14");
        REQUIRE(v.kind == PreparedValue::Kind::Double);
        REQUIRE(v.double_value == 3.14);
    }

    SECTION("negative double") {
        auto v = okValue(SqlParameterType::Double, "-0.5");
        REQUIRE(v.double_value == -0.5);
    }

    SECTION("integer-valued double") {
        auto v = okValue(SqlParameterType::Double, "10");
        REQUIRE(v.double_value == 10.0);
    }

    SECTION("scientific notation") {
        auto v = okValue(SqlParameterType::Double, "1e3");
        REQUIRE(v.double_value == 1000.0);
    }

    SECTION("trailing garbage is rejected") {
        auto outcome = c.convert(SqlParameterType::Double, "1.5x", true);
        REQUIRE_FALSE(outcome.ok);
    }

    SECTION("empty string rejected") {
        auto outcome = c.convert(SqlParameterType::Double, "", true);
        REQUIRE_FALSE(outcome.ok);
    }

    SECTION("classic SQL injection in numeric field errors") {
        auto outcome = c.convert(SqlParameterType::Double, "1.5 OR 1=1", true);
        REQUIRE_FALSE(outcome.ok);
    }
}

TEST_CASE("PreparedValueConverter: Boolean accepts true/false/1/0", "[prepared_value_converter]") {
    PreparedValueConverter c;

    SECTION("'true' parses to true") {
        auto v = okValue(SqlParameterType::Boolean, "true");
        REQUIRE(v.kind == PreparedValue::Kind::Bool);
        REQUIRE(v.bool_value == true);
    }

    SECTION("'TRUE' is case-insensitive") {
        auto v = okValue(SqlParameterType::Boolean, "TRUE");
        REQUIRE(v.bool_value == true);
    }

    SECTION("'false' parses to false") {
        auto v = okValue(SqlParameterType::Boolean, "false");
        REQUIRE(v.bool_value == false);
    }

    SECTION("'1' is true") {
        auto v = okValue(SqlParameterType::Boolean, "1");
        REQUIRE(v.bool_value == true);
    }

    SECTION("'0' is false") {
        auto v = okValue(SqlParameterType::Boolean, "0");
        REQUIRE(v.bool_value == false);
    }

    SECTION("'yes' is NOT accepted - keep parser strict") {
        auto outcome = c.convert(SqlParameterType::Boolean, "yes", true);
        REQUIRE_FALSE(outcome.ok);
    }

    SECTION("empty string rejected") {
        auto outcome = c.convert(SqlParameterType::Boolean, "", true);
        REQUIRE_FALSE(outcome.ok);
    }
}

TEST_CASE("PreparedValueConverter: Date parses YYYY-MM-DD", "[prepared_value_converter]") {
    PreparedValueConverter c;

    SECTION("valid date") {
        auto v = okValue(SqlParameterType::Date, "2024-03-15");
        REQUIRE(v.kind == PreparedValue::Kind::Date);
        REQUIRE(v.year == 2024);
        REQUIRE(v.month == 3);
        REQUIRE(v.day == 15);
    }

    SECTION("epoch") {
        auto v = okValue(SqlParameterType::Date, "1970-01-01");
        REQUIRE(v.year == 1970);
        REQUIRE(v.month == 1);
        REQUIRE(v.day == 1);
    }

    SECTION("zero-padded month/day required") {
        auto outcome = c.convert(SqlParameterType::Date, "2024-3-1", true);
        REQUIRE_FALSE(outcome.ok);
    }

    SECTION("invalid month rejected") {
        auto outcome = c.convert(SqlParameterType::Date, "2024-13-01", true);
        REQUIRE_FALSE(outcome.ok);
    }

    SECTION("invalid day rejected") {
        auto outcome = c.convert(SqlParameterType::Date, "2024-02-30", true);
        REQUIRE_FALSE(outcome.ok);
    }

    SECTION("garbage suffix rejected") {
        auto outcome = c.convert(SqlParameterType::Date, "2024-03-15 OR 1=1", true);
        REQUIRE_FALSE(outcome.ok);
    }

    SECTION("empty string maps to Null") {
        auto v = okValue(SqlParameterType::Date, "");
        REQUIRE(v.kind == PreparedValue::Kind::Null);
    }
}

TEST_CASE("PreparedValueConverter: Time parses HH:MM:SS[.ffffff]", "[prepared_value_converter]") {
    PreparedValueConverter c;

    SECTION("HH:MM:SS") {
        auto v = okValue(SqlParameterType::Time, "13:45:07");
        REQUIRE(v.kind == PreparedValue::Kind::Time);
        REQUIRE(v.hour == 13);
        REQUIRE(v.minute == 45);
        REQUIRE(v.second == 7);
        REQUIRE(v.micros == 0);
    }

    SECTION("with fractional seconds") {
        auto v = okValue(SqlParameterType::Time, "00:00:00.123456");
        REQUIRE(v.hour == 0);
        REQUIRE(v.minute == 0);
        REQUIRE(v.second == 0);
        REQUIRE(v.micros == 123456);
    }

    SECTION("hour boundary 23:59:59") {
        auto v = okValue(SqlParameterType::Time, "23:59:59");
        REQUIRE(v.hour == 23);
        REQUIRE(v.minute == 59);
        REQUIRE(v.second == 59);
    }

    SECTION("invalid hour rejected") {
        auto outcome = c.convert(SqlParameterType::Time, "24:00:00", true);
        REQUIRE_FALSE(outcome.ok);
    }

    SECTION("invalid minute rejected") {
        auto outcome = c.convert(SqlParameterType::Time, "12:60:00", true);
        REQUIRE_FALSE(outcome.ok);
    }

    SECTION("invalid second rejected") {
        auto outcome = c.convert(SqlParameterType::Time, "12:00:60", true);
        REQUIRE_FALSE(outcome.ok);
    }

    SECTION("garbage suffix rejected") {
        auto outcome = c.convert(SqlParameterType::Time, "12:00:00; DROP TABLE", true);
        REQUIRE_FALSE(outcome.ok);
    }

    SECTION("empty string maps to Null") {
        auto v = okValue(SqlParameterType::Time, "");
        REQUIRE(v.kind == PreparedValue::Kind::Null);
    }
}

TEST_CASE("PreparedValueConverter: Varchar passes raw through", "[prepared_value_converter]") {
    SECTION("ordinary string") {
        auto v = okValue(SqlParameterType::Varchar, "hello");
        REQUIRE(v.kind == PreparedValue::Kind::Varchar);
        REQUIRE(v.varchar == "hello");
    }

    SECTION("empty string is a valid Varchar value (not Null)") {
        // For string-shaped params the empty string is meaningful (a user
        // searching for "the empty product name") and is distinct from
        // an absent parameter. Bind it; do not coerce to NULL.
        auto v = okValue(SqlParameterType::Varchar, "");
        REQUIRE(v.kind == PreparedValue::Kind::Varchar);
        REQUIRE(v.varchar == "");
    }

    SECTION("contains SQL metacharacters - still just data") {
        auto v = okValue(SqlParameterType::Varchar, "Robert'); DROP TABLE Students;--");
        REQUIRE(v.kind == PreparedValue::Kind::Varchar);
        REQUIRE(v.varchar == "Robert'); DROP TABLE Students;--");
    }

    SECTION("contains NUL byte - preserved") {
        std::string s("a\0b", 3);
        auto v = okValue(SqlParameterType::Varchar, s);
        REQUIRE(v.varchar.size() == 3);
        REQUIRE(v.varchar == s);
    }

    SECTION("UTF-8 multibyte preserved") {
        auto v = okValue(SqlParameterType::Varchar, u8"café 北京");
        REQUIRE(v.varchar == u8"café 北京");
    }
}
