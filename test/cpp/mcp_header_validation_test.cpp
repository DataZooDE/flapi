#include <catch2/catch_test_macros.hpp>

#include <crow/utility.h>

#include "mcp_header_validation.hpp"

namespace flapi {
namespace test {

using flapi::mcp::decodeSentinel;
using flapi::mcp::numericEquals;
using flapi::mcp::headerMatches;

namespace {
std::string sentinel(const std::string& raw) {
    return "=?base64?" + crow::utility::base64encode(raw, raw.size()) + "?=";
}
}

TEST_CASE("decodeSentinel: plain ASCII passes through unchanged", "[mcp][headers]") {
    REQUIRE(decodeSentinel("tools/call") == "tools/call");
    REQUIRE(decodeSentinel("") == "");
    REQUIRE(decodeSentinel("42") == "42");
}

TEST_CASE("decodeSentinel: a sentinel-encoded value is base64-decoded", "[mcp][headers]") {
    REQUIRE(decodeSentinel(sentinel("Kundennummer")) == "Kundennummer");
    REQUIRE(decodeSentinel(sentinel("héllo wörld")) == "héllo wörld");
    REQUIRE(decodeSentinel(sentinel("with\nnewline")) == "with\nnewline");
    REQUIRE(decodeSentinel(sentinel(" leading/trailing ")) == " leading/trailing ");
}

TEST_CASE("decodeSentinel: a literal value that merely looks like a sentinel", "[mcp][headers]") {
    // Missing the closing marker -> not a sentinel, returned as-is.
    REQUIRE(decodeSentinel("=?base64?abc") == "=?base64?abc");
    // Uppercase marker is not the (lowercase) sentinel form.
    REQUIRE(decodeSentinel("=?BASE64?abc?=") == "=?BASE64?abc?=");
}

TEST_CASE("numericEquals: integer/decimal/whitespace equivalence", "[mcp][headers]") {
    REQUIRE(numericEquals("42", "42.0"));
    REQUIRE(numericEquals("42.0", "42"));
    REQUIRE(numericEquals(" 42 ", "42"));
    REQUIRE(numericEquals("-7", "-7.00"));
    REQUIRE_FALSE(numericEquals("42", "43"));
    REQUIRE_FALSE(numericEquals("abc", "abc"));   // non-numeric -> not numeric-equal
    REQUIRE_FALSE(numericEquals("42x", "42"));     // trailing garbage
}

TEST_CASE("headerMatches: exact, numeric, and sentinel paths", "[mcp][headers]") {
    // Exact string.
    REQUIRE(headerMatches("tools/call", "tools/call"));
    // Numeric equivalence for a mirrored integer param.
    REQUIRE(headerMatches("42", "42.0"));
    // Sentinel-encoded non-ASCII matches the plain body value.
    REQUIRE(headerMatches(sentinel("Kundennummer"), "Kundennummer"));
    // Mismatch.
    REQUIRE_FALSE(headerMatches("tools/call", "tools/list"));
    REQUIRE_FALSE(headerMatches(sentinel("Aaa"), "Bbb"));
}

} // namespace test
} // namespace flapi
