#include <catch2/catch_test_macros.hpp>

#include "cors_policy.hpp"

namespace flapi {
namespace test {

TEST_CASE("CorsPolicy: empty allowlist preserves wildcard (backward compat)",
          "[security][cors]") {
    CorsPolicy policy;
    auto result = policy.resolveAllowedOrigin("https://app.example.com", /*allow_origins=*/{});
    REQUIRE(result.has_value());
    REQUIRE(*result == "*");
}

TEST_CASE("CorsPolicy: wildcard token in allowlist returns wildcard",
          "[security][cors]") {
    CorsPolicy policy;
    auto result = policy.resolveAllowedOrigin("https://app.example.com",
                                              {"*"});
    REQUIRE(result.has_value());
    REQUIRE(*result == "*");
}

TEST_CASE("CorsPolicy: exact origin match is echoed back",
          "[security][cors]") {
    CorsPolicy policy;
    auto result = policy.resolveAllowedOrigin("https://app.example.com",
                                              {"https://app.example.com",
                                               "https://staging.example.com"});
    REQUIRE(result.has_value());
    REQUIRE(*result == "https://app.example.com");
}

TEST_CASE("CorsPolicy: non-matching origin yields nullopt",
          "[security][cors]") {
    // Browsers see no ACAO header → request is blocked. This is the whole
    // point of the allowlist.
    CorsPolicy policy;
    auto result = policy.resolveAllowedOrigin("https://evil.example.com",
                                              {"https://app.example.com"});
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("CorsPolicy: empty request origin with non-wildcard allowlist yields nullopt",
          "[security][cors]") {
    // Same-origin requests typically don't send an Origin header; the
    // policy must not invent one for them.
    CorsPolicy policy;
    auto result = policy.resolveAllowedOrigin("",
                                              {"https://app.example.com"});
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("CorsPolicy: empty request origin with wildcard allowlist returns wildcard",
          "[security][cors]") {
    // Same-origin demo path still gets the wildcard, preserving current
    // ease-of-use defaults when the allowlist is unset.
    CorsPolicy policy;
    auto result = policy.resolveAllowedOrigin("", /*allow_origins=*/{});
    REQUIRE(result.has_value());
    REQUIRE(*result == "*");
}

TEST_CASE("CorsPolicy: origin match is case-sensitive",
          "[security][cors]") {
    // Per the spec, origins are compared byte-for-byte (case-sensitive scheme
    // and host). Tolerating case differences would weaken the allowlist.
    CorsPolicy policy;
    auto result = policy.resolveAllowedOrigin("https://App.Example.com",
                                              {"https://app.example.com"});
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("CorsPolicy: wildcard mixed with explicit entries collapses to wildcard",
          "[security][cors]") {
    // Operators who write `["*", "https://x.com"]` get the wildcard semantics
    // they implicitly asked for. The misconfiguration is harmless.
    CorsPolicy policy;
    auto result = policy.resolveAllowedOrigin("https://anything.com",
                                              {"https://x.com", "*"});
    REQUIRE(result.has_value());
    REQUIRE(*result == "*");
}

} // namespace test
} // namespace flapi
