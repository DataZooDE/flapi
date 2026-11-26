#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <jwt-cpp/jwt.h>

#include "bearer_token_verifier.hpp"

using namespace flapi;

namespace {

const std::string kSecret = "a-long-enough-unit-test-secret";

auto now() { return std::chrono::system_clock::now(); }

jwt::builder<jwt::traits::kazuho_picojson> base() {
    return jwt::create()
        .set_subject("alice")
        .set_issued_at(now())
        .set_expires_at(now() + std::chrono::minutes(10));
}

BearerVerifierConfig config() {
    BearerVerifierConfig c;
    c.secret = kSecret;
    return c;
}

}  // namespace

TEST_CASE("VerifyBearerToken accepts a valid token and reads the identity", "[auth][jwt][bearer]") {
    const auto token = base()
                           .set_payload_claim("email", jwt::claim(std::string("alice@example.com")))
                           .set_payload_claim("roles", jwt::claim(std::set<std::string>{"a", "b"}))
                           .sign(jwt::algorithm::hs256{kSecret});
    const auto id = VerifyBearerToken(token, config());
    REQUIRE(id);
    CHECK(id->username == "alice");
    CHECK(id->email == "alice@example.com");
    CHECK(id->roles == std::vector<std::string>{"a", "b"});
}

TEST_CASE("VerifyBearerToken: roles are optional, a string is one role", "[auth][jwt][bearer]") {
    auto id = VerifyBearerToken(base().sign(jwt::algorithm::hs256{kSecret}), config());
    REQUIRE(id);
    CHECK(id->roles.empty());
    CHECK(id->email.empty());

    id = VerifyBearerToken(
        base().set_payload_claim("roles", jwt::claim(std::string("solo"))).sign(jwt::algorithm::hs256{kSecret}),
        config());
    REQUIRE(id);
    CHECK(id->roles == std::vector<std::string>{"solo"});
}

TEST_CASE("VerifyBearerToken: exp", "[auth][jwt][bearer]") {
    const auto no_exp = jwt::create().set_subject("alice").sign(jwt::algorithm::hs256{kSecret});
    std::string reason;
    CHECK_FALSE(VerifyBearerToken(no_exp, config(), &reason));
    CHECK(reason.find("exp") != std::string::npos);

    auto lax = config();
    lax.require_exp = false;
    CHECK(VerifyBearerToken(no_exp, lax));

    const auto expired = jwt::create()
                             .set_subject("alice")
                             .set_expires_at(now() - std::chrono::hours(1))
                             .sign(jwt::algorithm::hs256{kSecret});
    CHECK_FALSE(VerifyBearerToken(expired, config()));
    CHECK_FALSE(VerifyBearerToken(expired, lax));
}

TEST_CASE("VerifyBearerToken: nbf and iat in the future are refused", "[auth][jwt][bearer]") {
    const auto later = now() + std::chrono::hours(1);
    CHECK_FALSE(VerifyBearerToken(base().set_not_before(later).sign(jwt::algorithm::hs256{kSecret}), config()));
    CHECK_FALSE(VerifyBearerToken(base().set_issued_at(later).sign(jwt::algorithm::hs256{kSecret}), config()));
}

TEST_CASE("VerifyBearerToken: issuer", "[auth][jwt][bearer]") {
    const auto with_iss = base().set_issuer("idp-a").sign(jwt::algorithm::hs256{kSecret});
    const auto without_iss = base().sign(jwt::algorithm::hs256{kSecret});
    // Unset: not checked.
    CHECK(VerifyBearerToken(with_iss, config()));
    CHECK(VerifyBearerToken(without_iss, config()));
    auto strict = config();
    strict.issuer = "idp-a";
    CHECK(VerifyBearerToken(with_iss, strict));
    CHECK_FALSE(VerifyBearerToken(without_iss, strict));
    strict.issuer = "idp-b";
    CHECK_FALSE(VerifyBearerToken(with_iss, strict));
}

TEST_CASE("VerifyBearerToken: audience", "[auth][jwt][bearer]") {
    auto c = config();
    c.audiences = {"api-1", "api-2"};
    CHECK(VerifyBearerToken(base().set_audience("api-2").sign(jwt::algorithm::hs256{kSecret}), c));
    CHECK(VerifyBearerToken(
        base().set_audience(picojson::array{picojson::value("x"), picojson::value("api-1")})
            .sign(jwt::algorithm::hs256{kSecret}),
        c));
    CHECK_FALSE(VerifyBearerToken(base().set_audience("api-3").sign(jwt::algorithm::hs256{kSecret}), c));
    CHECK_FALSE(VerifyBearerToken(base().sign(jwt::algorithm::hs256{kSecret}), c));
}

TEST_CASE("VerifyBearerToken: signature, algorithm, secret and sub", "[auth][jwt][bearer]") {
    CHECK_FALSE(VerifyBearerToken(base().sign(jwt::algorithm::hs256{"another-secret"}), config()));
    CHECK_FALSE(VerifyBearerToken(base().sign(jwt::algorithm::hs384{kSecret}), config()));
    CHECK_FALSE(VerifyBearerToken(base().sign(jwt::algorithm::none{}), config()));
    CHECK_FALSE(VerifyBearerToken("not.a.jwt", config()));

    // An empty key verifies anything signed with it: refused outright.
    auto empty = config();
    empty.secret.clear();
    CHECK_FALSE(VerifyBearerToken(base().sign(jwt::algorithm::hs256{""}), empty));

    const auto no_sub = jwt::create()
                            .set_expires_at(now() + std::chrono::minutes(10))
                            .sign(jwt::algorithm::hs256{kSecret});
    CHECK_FALSE(VerifyBearerToken(no_sub, config()));
}
