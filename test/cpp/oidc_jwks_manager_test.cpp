// OIDCJWKSManager under concurrency, the refetch cooldown, EC key parsing, and
// the OIDC handler registry (#281, #284).
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/obj_mac.h>
#include <string>
#include <thread>
#include <vector>

#include "oidc_handler_registry.hpp"
#include "oidc_jwks_manager.hpp"

using namespace flapi;

namespace {

std::string b64url(const std::string& raw) {
    static const char* kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    std::size_t i = 0;
    for (; i + 2 < raw.size(); i += 3) {
        const unsigned v = (static_cast<unsigned char>(raw[i]) << 16) | (static_cast<unsigned char>(raw[i + 1]) << 8) |
                           static_cast<unsigned char>(raw[i + 2]);
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += kAlphabet[(v >> 6) & 63];
        out += kAlphabet[v & 63];
    }
    if (i + 1 == raw.size()) {
        const unsigned v = static_cast<unsigned char>(raw[i]) << 16;
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
    } else if (i + 2 == raw.size()) {
        const unsigned v = (static_cast<unsigned char>(raw[i]) << 16) | (static_cast<unsigned char>(raw[i + 1]) << 8);
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += kAlphabet[(v >> 6) & 63];
    }
    return out;
}

// An RSA JWK. The modulus need not be a real one for parsing and lookups.
std::string rsaJwk(const std::string& kid) {
    std::string n(256, '\x5a');
    n[0] = '\xc1';
    n[255] = '\x01';
    return R"({"kty":"RSA","use":"sig","alg":"RS256","kid":")" + kid + R"(","n":")" + b64url(n) +
           R"(","e":"AQAB"})";
}

std::string jwks(const std::vector<std::string>& jwk_objects) {
    std::string out = R"({"keys":[)";
    for (std::size_t i = 0; i < jwk_objects.size(); ++i) {
        out += (i == 0 ? "" : ",") + jwk_objects[i];
    }
    return out + "]}";
}

struct EcPoint {
    std::string x, y;
};

EcPoint generateP256Point() {
    EC_KEY* key = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
    REQUIRE(EC_KEY_generate_key(key) == 1);
    BIGNUM* x = BN_new();
    BIGNUM* y = BN_new();
    REQUIRE(EC_POINT_get_affine_coordinates(EC_KEY_get0_group(key), EC_KEY_get0_public_key(key), x, y, nullptr) == 1);
    EcPoint p{std::string(32, '\0'), std::string(32, '\0')};
    BN_bn2binpad(x, reinterpret_cast<unsigned char*>(p.x.data()), 32);
    BN_bn2binpad(y, reinterpret_cast<unsigned char*>(p.y.data()), 32);
    BN_free(x);
    BN_free(y);
    EC_KEY_free(key);
    return p;
}

constexpr const char* kUrl = "https://idp.example.com/jwks";

}  // namespace

TEST_CASE("OIDCJWKSManager: concurrent first lookups share one fetch", "[oidc][jwks][concurrency]") {
    OIDCJWKSManager manager;
    std::atomic<int> fetches{0};
    manager.setFetcher([&](const std::string&) -> std::optional<std::string> {
        ++fetches;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return jwks({rsaJwk("k1")});
    });
    std::atomic<int> found{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 32; ++i) {
        threads.emplace_back([&] {
            if (manager.getKey("k1", kUrl)) {
                ++found;
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    REQUIRE(found == 32);
    REQUIRE(fetches == 1);
    REQUIRE(manager.fetchCount() == 1);
}

TEST_CASE("OIDCJWKSManager: unknown kids wait for the cooldown", "[oidc][jwks][cooldown]") {
    OIDCJWKSManager manager;
    std::atomic<int> fetches{0};
    manager.setFetcher([&](const std::string&) -> std::optional<std::string> {
        ++fetches;
        return jwks({rsaJwk("k1")});
    });

    SECTION("within the cooldown no unknown kid reaches the issuer") {
        manager.setRefetchCooldown(std::chrono::seconds(60));
        REQUIRE(manager.getKey("k1", kUrl));
        // Catch2 assertions are not thread-safe: threads only count.
        std::atomic<int> wrongly_found{0};
        std::vector<std::thread> threads;
        for (int i = 0; i < 16; ++i) {
            threads.emplace_back([&, i] {
                for (int j = 0; j < 20; ++j) {
                    if (manager.getKey("ghost-" + std::to_string(i * 100 + j), kUrl)) {
                        ++wrongly_found;
                    }
                }
            });
        }
        for (auto& t : threads) {
            t.join();
        }
        REQUIRE(wrongly_found == 0);
        REQUIRE(fetches == 1);
        REQUIRE(manager.getKey("k1", kUrl));
    }

    SECTION("cooldown 0 refetches for every unknown kid, one fetch per lookup") {
        manager.setRefetchCooldown(std::chrono::seconds(0));
        REQUIRE(manager.getKey("k1", kUrl));
        REQUIRE_FALSE(manager.getKey("ghost", kUrl));
        REQUIRE_FALSE(manager.getKey("ghost", kUrl));
        REQUIRE(fetches == 3);
    }

    SECTION("a kid that appears after rotation is found once the cooldown has passed") {
        manager.setRefetchCooldown(std::chrono::seconds(1));
        REQUIRE(manager.getKey("k1", kUrl));
        manager.setFetcher([&](const std::string&) -> std::optional<std::string> {
            ++fetches;
            return jwks({rsaJwk("k1"), rsaJwk("k2")});
        });
        REQUIRE_FALSE(manager.getKey("k2", kUrl));   // within the cooldown: negative cache
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        REQUIRE(manager.getKey("k2", kUrl));
        REQUIRE(fetches == 2);
    }
}

TEST_CASE("OIDCJWKSManager: a failed fetch is retried after a short delay, not per request",
          "[oidc][jwks][cooldown]") {
    OIDCJWKSManager manager;
    manager.setRefetchCooldown(std::chrono::seconds(60));
    std::atomic<int> fetches{0};
    manager.setFetcher([&](const std::string&) -> std::optional<std::string> {
        ++fetches;
        return std::nullopt;
    });
    for (int i = 0; i < 50; ++i) {
        REQUIRE_FALSE(manager.getKey("k1", kUrl));
    }
    REQUIRE(fetches == 1);
}

TEST_CASE("OIDCJWKSManager: a key stays valid while the set is replaced", "[oidc][jwks][concurrency]") {
    OIDCJWKSManager manager;
    manager.setRefetchCooldown(std::chrono::seconds(0));
    manager.setFetcher([](const std::string&) -> std::optional<std::string> { return jwks({rsaJwk("k1")}); });
    auto held = manager.getKey("k1", kUrl);
    REQUIRE(held);

    std::atomic<bool> stop{false};
    std::thread writer([&] {
        while (!stop) {
            manager.refreshJWKS(kUrl);
        }
    });
    std::vector<std::thread> readers;
    std::atomic<int> reads{0};
    for (int i = 0; i < 4; ++i) {
        readers.emplace_back([&] {
            for (int j = 0; j < 500; ++j) {
                auto key = manager.getKey("k1", kUrl);
                // Before #281 this read memory a concurrent refresh had freed.
                if (key && key->kid == "k1" && EVP_PKEY_base_id(key->evp_key) == EVP_PKEY_RSA) {
                    ++reads;
                }
            }
        });
    }
    for (auto& r : readers) {
        r.join();
    }
    stop = true;
    writer.join();
    REQUIRE(reads == 2000);
    // The key taken before all the refreshes is still intact.
    REQUIRE(held->kid == "k1");
    REQUIRE(EVP_PKEY_base_id(held->evp_key) == EVP_PKEY_RSA);
}

TEST_CASE("OIDCJWKSManager parses EC keys and skips what it cannot use", "[oidc][jwks][ec]") {
    OIDCJWKSManager manager;
    const auto p = generateP256Point();
    const std::string ec = R"({"kty":"EC","use":"sig","kid":"ec1","crv":"P-256","x":")" + b64url(p.x) +
                           R"(","y":")" + b64url(p.y) + R"("})";
    std::string off_curve_y = p.y;
    off_curve_y[31] ^= 0x01;
    const std::string bad_point = R"({"kty":"EC","kid":"ec-bad","crv":"P-256","x":")" + b64url(p.x) +
                                  R"(","y":")" + b64url(off_curve_y) + R"("})";
    const std::string wrong_curve = R"({"kty":"EC","kid":"ec-384","crv":"P-384","x":")" + b64url(p.x) +
                                    R"(","y":")" + b64url(p.y) + R"("})";
    const std::string enc = R"({"kty":"RSA","use":"enc","kid":"enc1","n":"AQAB","e":"AQAB"})";
    const std::string oct = R"({"kty":"oct","kid":"hmac","k":"c2VjcmV0"})";

    auto keys = manager.parseJWKSResponse(jwks({ec, bad_point, wrong_curve, enc, oct, rsaJwk("rsa1")}));
    REQUIRE(keys);
    std::vector<std::string> kids;
    for (const auto& k : *keys) {
        kids.push_back(k.kid);
    }
    REQUIRE(kids == std::vector<std::string>{"ec1", "rsa1"});
    REQUIRE((*keys)[0].crv == "P-256");
    REQUIRE((*keys)[0].alg.empty());
    REQUIRE(EVP_PKEY_base_id((*keys)[0].evp_key) == EVP_PKEY_EC);
}

TEST_CASE("OIDCHandlerRegistry keys handlers on the whole effective config", "[oidc][registry]") {
    auto& registry = OIDCHandlerRegistry::instance();
    registry.clear();
    OIDCConfig a;
    a.issuer_url = "https://idp.example.com";
    a.client_id = "flapi";
    a.allowed_audiences = {"api-a"};
    OIDCConfig b = a;
    b.allowed_audiences = {"api-b"};

    const auto handler_a = registry.handlerFor(a);
    REQUIRE(registry.handlerFor(a) == handler_a);
    REQUIRE(registry.handlerFor(b) != handler_a);   // used to share a's handler

    OIDCConfig claims = a;
    claims.username_claim = "email";
    REQUIRE(registry.handlerFor(claims) != handler_a);

    // A preset that expands to the same configuration is the same handler.
    OIDCConfig preset;
    preset.provider_type = "keycloak";
    preset.domain = "kc.example.com";
    preset.realm = "acme";
    preset.client_id = "flapi";
    OIDCConfig spelled;
    spelled.issuer_url = "https://kc.example.com/realms/acme";
    spelled.client_id = "flapi";
    spelled.provider_type = "keycloak";
    REQUIRE(registry.handlerFor(preset) == registry.handlerFor(spelled));

    // Four handlers on two issuers: the keys are fetched once per issuer.
    REQUIRE(registry.handlerCount() == 4);
    REQUIRE(registry.providerCount() == 2);

    // Concurrent first lookups of one config agree on one handler.
    registry.clear();
    std::vector<std::shared_ptr<OIDCAuthHandler>> seen(16);
    std::vector<std::thread> threads;
    for (std::size_t i = 0; i < seen.size(); ++i) {
        threads.emplace_back([&, i] { seen[i] = registry.handlerFor(a); });
    }
    for (auto& t : threads) {
        t.join();
    }
    for (const auto& h : seen) {
        REQUIRE(h == seen.front());
    }
    REQUIRE(registry.handlerCount() == 1);
    registry.clear();
}
