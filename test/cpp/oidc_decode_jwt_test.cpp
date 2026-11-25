#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#include "oidc_auth_handler.hpp"

using namespace flapi;

// Issue #273: OIDC refused every valid token.
//
// decodeJWT parsed the header and payload with crow::json::load and returned
// std::make_pair(header, payload). A COPY of a crow rvalue does not own the
// text buffer it points into; the originals did, and freed it on return. The
// later header.has("alg") read freed memory, so the log said
// "JWT header missing 'alg' field" for a header that had one.
//
// DecodedJWT keeps the owning roots and cannot be copied. These tests read the
// parsed values after the decoding scope is gone and the heap has been
// churned, which is where the old code read garbage. Under ASAN the old code
// is a heap-use-after-free on the first header access.
namespace {

std::string b64url(const std::string& raw) {
    std::string out = crow::utility::base64encode_urlsafe(raw, raw.size());
    while (!out.empty() && out.back() == '=') {
        out.pop_back();
    }
    return out;
}

std::string makeToken(const std::string& header, const std::string& payload) {
    return b64url(header) + "." + b64url(payload) + ".c2ln";
}

const std::string HEADER = R"({"alg":"RS256","typ":"JWT","kid":"k1"})";
const std::string PAYLOAD =
    R"({"iss":"http://127.0.0.1:1","sub":"user-42","aud":"flapi","exp":4102444800,)"
    R"("roles":["analyst","viewer"],"realm_access":{"roles":["admin"]}})";

// Reuse freed memory, so a dangling read sees something other than the JSON.
void churnHeap() {
    std::vector<std::string> junk;
    for (int i = 0; i < 256; ++i) {
        junk.emplace_back(64 + (i % 64), static_cast<char>('A' + (i % 26)));
    }
}

}  // namespace

static_assert(!std::is_copy_constructible_v<DecodedJWT>,
              "a copied crow rvalue does not own its buffer (#273)");
static_assert(!std::is_copy_assignable_v<DecodedJWT>,
              "a copied crow rvalue does not own its buffer (#273)");
static_assert(std::is_nothrow_move_constructible_v<DecodedJWT>);

TEST_CASE("decoded header and payload stay readable after decoding returns",
          "[oidc][security][decode]") {
    std::optional<DecodedJWT> decoded;
    {
        const std::string token = makeToken(HEADER, PAYLOAD);
        decoded.emplace(*DecodedJWT::Decode(token));
    }
    churnHeap();

    const auto& header = decoded->header();
    REQUIRE(header.has("alg"));
    REQUIRE(std::string(header["alg"].s()) == "RS256");
    REQUIRE(header.has("kid"));
    REQUIRE(std::string(header["kid"].s()) == "k1");

    const auto& payload = decoded->payload();
    REQUIRE(std::string(payload["sub"].s()) == "user-42");
    REQUIRE(payload["exp"].i() == 4102444800LL);
    REQUIRE(payload["roles"].size() == 2);
    REQUIRE(std::string(payload["realm_access"]["roles"][0].s()) == "admin");
}

TEST_CASE("a moved DecodedJWT keeps its values", "[oidc][security][decode]") {
    auto first = DecodedJWT::Decode(makeToken(HEADER, PAYLOAD));
    REQUIRE(first);
    DecodedJWT moved(std::move(*first));
    first.reset();
    churnHeap();
    REQUIRE(std::string(moved.header()["alg"].s()) == "RS256");
    REQUIRE(std::string(moved.payload()["sub"].s()) == "user-42");
}

TEST_CASE("a header with kid first still yields alg", "[oidc][decode]") {
    // The issue's log said 'kid' was missing when kid was the first key.
    auto decoded = DecodedJWT::Decode(
        makeToken(R"({"kid":"k9","alg":"RS256","typ":"JWT"})", PAYLOAD));
    REQUIRE(decoded);
    churnHeap();
    REQUIRE(std::string(decoded->header()["kid"].s()) == "k9");
    REQUIRE(std::string(decoded->header()["alg"].s()) == "RS256");
}

TEST_CASE("malformed tokens do not decode", "[oidc][decode]") {
    const std::string h = b64url(HEADER);
    const std::string p = b64url(PAYLOAD);
    CHECK_FALSE(DecodedJWT::Decode(""));
    CHECK_FALSE(DecodedJWT::Decode("not-a-jwt"));
    CHECK_FALSE(DecodedJWT::Decode(h + "." + p));                 // two parts
    CHECK_FALSE(DecodedJWT::Decode(h + "." + p + ".s.extra"));    // four parts
    CHECK_FALSE(DecodedJWT::Decode("." + p + ".s"));              // empty header
    CHECK_FALSE(DecodedJWT::Decode(h + "..s"));                   // empty payload
    CHECK_FALSE(DecodedJWT::Decode(b64url("not json") + "." + p + ".s"));
    CHECK_FALSE(DecodedJWT::Decode(b64url("[1,2]") + "." + p + ".s"));  // not an object
    CHECK_FALSE(DecodedJWT::Decode(h + "." + b64url("\"str\"") + ".s"));
}

TEST_CASE("an empty signature part still decodes (alg none is refused later)",
          "[oidc][decode]") {
    REQUIRE(DecodedJWT::Decode(b64url(HEADER) + "." + b64url(PAYLOAD) + "."));
}
