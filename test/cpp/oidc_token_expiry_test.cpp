#include <catch2/catch_test_macros.hpp>

#include <chrono>

#include "oidc_auth_handler.hpp"

using namespace flapi;

// OIDC token expiry never fired.
//
// `exp` and `iat` are Unix epoch seconds - absolute instants on the wall
// clock. They were stored in a std::chrono::steady_clock::time_point, whose
// epoch is arbitrary and on Linux is the boot time. So an `exp` of 1.8e9
// became a time_point roughly 57 years after boot, and
//
//     steady_clock::now() > claims.expires_at + skew
//
// was false for every token that will ever be presented. verify_expiration
// defaults to TRUE, so operators had expiry switched on, saw no errors, and
// were accepting tokens that expired at any point in the past.
//
// Measured before the fix: a token with exp = 946684800 (1 January 2000) was
// reported as not expired.
//
// The same value flowed into MCPSession::token_expires_at, which repeated the
// comparison and was therefore equally inert.
namespace {

OIDCAuthHandler::Config testConfig(int skew_seconds = 0) {
    OIDCAuthHandler::Config cfg;
    cfg.issuer_url = "https://issuer.example.com";
    cfg.client_id = "flapi-test";
    cfg.verify_expiration = true;
    cfg.clock_skew_seconds = skew_seconds;
    return cfg;
}

// Build claims the way validateToken does: from an epoch-seconds value.
OIDCTokenClaims claimsExpiringAt(std::int64_t unix_seconds) {
    OIDCTokenClaims claims;
    claims.expires_at =
        std::chrono::system_clock::time_point(std::chrono::seconds(unix_seconds));
    return claims;
}

std::int64_t nowEpochSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

}  // namespace

TEST_CASE("a token that expired long ago is expired", "[oidc][security][expiry]") {
    OIDCAuthHandler handler(testConfig());
    // 1 January 2000. The exact case that was accepted before.
    REQUIRE(handler.isTokenExpired(claimsExpiringAt(946684800)));
}

TEST_CASE("a token that expired a moment ago is expired", "[oidc][security][expiry]") {
    OIDCAuthHandler handler(testConfig(/*skew_seconds=*/0));
    REQUIRE(handler.isTokenExpired(claimsExpiringAt(nowEpochSeconds() - 60)));
}

TEST_CASE("a token valid for another hour is not expired", "[oidc][security][expiry]") {
    // The other half of the contract: the fix must not reject live tokens.
    OIDCAuthHandler handler(testConfig());
    REQUIRE_FALSE(handler.isTokenExpired(claimsExpiringAt(nowEpochSeconds() + 3600)));
}

TEST_CASE("clock skew is honoured on both sides", "[oidc][security][expiry]") {
    OIDCAuthHandler handler(testConfig(/*skew_seconds=*/300));

    SECTION("just inside the skew window is still accepted") {
        REQUIRE_FALSE(handler.isTokenExpired(claimsExpiringAt(nowEpochSeconds() - 60)));
    }

    SECTION("beyond the skew window is expired") {
        // Skew is tolerance, not an amnesty - it must still expire eventually.
        REQUIRE(handler.isTokenExpired(claimsExpiringAt(nowEpochSeconds() - 3600)));
    }
}
