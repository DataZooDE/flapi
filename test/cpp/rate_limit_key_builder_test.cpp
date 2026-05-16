#include <catch2/catch_test_macros.hpp>

#include "rate_limit_key_builder.hpp"

namespace flapi {
namespace test {

namespace {

constexpr const char* kIp = "203.0.113.5";
constexpr const char* kBearer = "Bearer abc.def.ghi";
constexpr const char* kBearer2 = "Bearer xyz.uvw.rst";
constexpr const char* kPath = "/customers";

} // namespace

TEST_CASE("RateLimitKeyBuilder: ip strategy ignores the Authorization header",
          "[security][ratelimit]") {
    RateLimitKeyBuilder builder;
    auto with_auth = builder.buildKey(RateLimitKeyStrategy::Ip, kIp, kBearer, kPath);
    auto without_auth = builder.buildKey(RateLimitKeyStrategy::Ip, kIp, "", kPath);
    REQUIRE(with_auth == without_auth);
    REQUIRE(with_auth.find(kIp) != std::string::npos);
    REQUIRE(with_auth.find(kPath) != std::string::npos);
}

TEST_CASE("RateLimitKeyBuilder: user strategy yields different keys for different tokens",
          "[security][ratelimit]") {
    // Two callers from the same IP with different bearer tokens must
    // end up in different buckets when key=user.
    RateLimitKeyBuilder builder;
    auto a = builder.buildKey(RateLimitKeyStrategy::User, kIp, kBearer, kPath);
    auto b = builder.buildKey(RateLimitKeyStrategy::User, kIp, kBearer2, kPath);
    REQUIRE(a != b);
}

TEST_CASE("RateLimitKeyBuilder: user strategy ignores the client IP",
          "[security][ratelimit]") {
    // One user reaching the same endpoint from two IPs (mobile + Wi-Fi)
    // must stay in a single bucket when key=user.
    RateLimitKeyBuilder builder;
    auto a = builder.buildKey(RateLimitKeyStrategy::User, "10.0.0.1", kBearer, kPath);
    auto b = builder.buildKey(RateLimitKeyStrategy::User, "10.0.0.2", kBearer, kPath);
    REQUIRE(a == b);
}

TEST_CASE("RateLimitKeyBuilder: user strategy with no auth header maps to anonymous bucket",
          "[security][ratelimit]") {
    // Anonymous callers must not share a bucket with any real user, but
    // they DO share a single anonymous bucket among themselves — this is
    // intentional: anonymous traffic is the easiest to abuse, so the
    // tightest grouping is the safest default.
    RateLimitKeyBuilder builder;
    auto a = builder.buildKey(RateLimitKeyStrategy::User, kIp, "", kPath);
    auto b = builder.buildKey(RateLimitKeyStrategy::User, "10.0.0.99", "", kPath);
    REQUIRE(a == b);
    REQUIRE(a.find("anonymous") != std::string::npos);
}

TEST_CASE("RateLimitKeyBuilder: user-or-ip falls back to ip when auth absent",
          "[security][ratelimit]") {
    RateLimitKeyBuilder builder;
    auto user_or_ip_anon = builder.buildKey(RateLimitKeyStrategy::UserOrIp, kIp, "", kPath);
    auto ip_only = builder.buildKey(RateLimitKeyStrategy::Ip, kIp, "", kPath);
    REQUIRE(user_or_ip_anon == ip_only);
}

TEST_CASE("RateLimitKeyBuilder: user-or-ip uses auth when present",
          "[security][ratelimit]") {
    RateLimitKeyBuilder builder;
    auto user_or_ip_auth = builder.buildKey(RateLimitKeyStrategy::UserOrIp, kIp, kBearer, kPath);
    auto user_only_auth = builder.buildKey(RateLimitKeyStrategy::User, kIp, kBearer, kPath);
    REQUIRE(user_or_ip_auth == user_only_auth);
}

TEST_CASE("RateLimitKeyBuilder: path is part of every key for per-endpoint isolation",
          "[security][ratelimit]") {
    // Two endpoints hit by the same caller stay in separate buckets.
    RateLimitKeyBuilder builder;
    auto a = builder.buildKey(RateLimitKeyStrategy::Ip, kIp, "", "/foo");
    auto b = builder.buildKey(RateLimitKeyStrategy::Ip, kIp, "", "/bar");
    REQUIRE(a != b);
}

TEST_CASE("RateLimitKeyBuilder: token derivation is stable across calls",
          "[security][ratelimit]") {
    // The hash must be deterministic; otherwise the same caller's
    // requests would land in different buckets and the counter would
    // never decrement properly.
    RateLimitKeyBuilder builder;
    auto a = builder.buildKey(RateLimitKeyStrategy::User, kIp, kBearer, kPath);
    auto b = builder.buildKey(RateLimitKeyStrategy::User, kIp, kBearer, kPath);
    REQUIRE(a == b);
}

TEST_CASE("RateLimitKeyBuilder: hashed auth token does not appear verbatim in the key",
          "[security][ratelimit]") {
    // The raw Authorization header value is sensitive; the key is
    // logged at debug level and persisted in the per-process map. It
    // must not contain the bearer token in plaintext.
    RateLimitKeyBuilder builder;
    auto key = builder.buildKey(RateLimitKeyStrategy::User, kIp,
                                "Bearer super-secret-token-abc123", kPath);
    REQUIRE(key.find("super-secret-token-abc123") == std::string::npos);
}

TEST_CASE("RateLimitKeyStrategy::parse: known names map to the right enum",
          "[security][ratelimit]") {
    REQUIRE(RateLimitKeyStrategyUtils::parse("ip") == RateLimitKeyStrategy::Ip);
    REQUIRE(RateLimitKeyStrategyUtils::parse("user") == RateLimitKeyStrategy::User);
    REQUIRE(RateLimitKeyStrategyUtils::parse("user-or-ip") == RateLimitKeyStrategy::UserOrIp);
}

TEST_CASE("RateLimitKeyStrategy::parse: empty or unknown falls back to ip (backward compat)",
          "[security][ratelimit]") {
    REQUIRE(RateLimitKeyStrategyUtils::parse("") == RateLimitKeyStrategy::Ip);
    REQUIRE(RateLimitKeyStrategyUtils::parse("garbage") == RateLimitKeyStrategy::Ip);
}

} // namespace test
} // namespace flapi
