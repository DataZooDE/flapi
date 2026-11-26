#pragma once

#include <string>
#include <vector>
#include <optional>
#include <memory>
#include <mutex>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/pem.h>
#include <crow.h>

namespace flapi {

/**
 * Represents a single key from JWKS
 */
struct JWKSKey {
    std::string kid;              // Key ID
    std::string kty;              // Key type: RSA or EC
    std::string use;              // Use (sig for signature)
    std::string alg;              // The JWK's `alg`, or empty when it names none
    std::string crv;              // EC curve: P-256, P-384 or P-521 (empty for RSA)
    std::string n;                // Modulus (base64url encoded)
    std::string e;                // Exponent (base64url encoded)
    EVP_PKEY* evp_key = nullptr;  // OpenSSL EVP_PKEY for verification

    JWKSKey() = default;

    ~JWKSKey() {
        if (evp_key) {
            EVP_PKEY_free(evp_key);
        }
    }

    // Prevent copying
    JWKSKey(const JWKSKey&) = delete;
    JWKSKey& operator=(const JWKSKey&) = delete;

    // Allow moving
    JWKSKey(JWKSKey&& other) noexcept
        : kid(std::move(other.kid)), kty(std::move(other.kty)),
          use(std::move(other.use)), alg(std::move(other.alg)),
          crv(std::move(other.crv)), n(std::move(other.n)), e(std::move(other.e)),
          evp_key(other.evp_key) {
        other.evp_key = nullptr;
    }

    JWKSKey& operator=(JWKSKey&& other) noexcept {
        if (this != &other) {
            if (evp_key) {
                EVP_PKEY_free(evp_key);
            }
            kid = std::move(other.kid);
            kty = std::move(other.kty);
            use = std::move(other.use);
            alg = std::move(other.alg);
            crv = std::move(other.crv);
            n = std::move(other.n);
            e = std::move(other.e);
            evp_key = other.evp_key;
            other.evp_key = nullptr;
        }
        return *this;
    }
};

/**
 * OIDCJWKSManager fetches and caches JWKS (JSON Web Key Sets), per JWKS URL.
 *
 * Thread-safe. A key set is held as a `shared_ptr<const vector<JWKSKey>>`
 * and replaced, never edited, when it is refetched; getKey hands out an
 * aliasing `shared_ptr<const JWKSKey>` that keeps its set alive. It used to
 * return `&key` into a vector a concurrent refresh replaced (and freed) -
 * a use-after-free on every key rotation under load.
 *
 * Fetches are bounded (#284). One getKey fetches at most once, concurrent
 * callers share one fetch, and after a fetch the next one for an unknown kid
 * waits for the refetch cooldown (default 60 s; a failed fetch is retried
 * after min(cooldown, 5 s)). Within the cooldown an unknown kid is refused
 * from a negative cache instead of reaching the issuer, so a flood of tokens
 * with invented kids costs the issuer one request per cooldown.
 */
class OIDCJWKSManager {
public:
    /// The JWKS document at `url`, or nullopt when it cannot be fetched. The
    /// default GETs it with HTTPClient and wants status 200.
    using Fetcher = std::function<std::optional<std::string>(const std::string& url)>;

    OIDCJWKSManager();
    ~OIDCJWKSManager();

    /**
     * Fetch the JWKS now, whatever the cooldown (shares a fetch already
     * running). true when the key set was replaced.
     */
    bool refreshJWKS(const std::string& jwks_url);

    /**
     * The key `kid`, or nullptr. Fetches the JWKS at most once, and only when
     * it was never fetched, has outlived the cache TTL, or does not hold
     * `kid` - each subject to the refetch cooldown.
     */
    std::shared_ptr<const JWKSKey> getKey(const std::string& kid, const std::string& jwks_url);

    /**
     * Check if JWKS cache needs refresh (never fetched, or older than the TTL)
     */
    bool needsRefresh(const std::string& jwks_url) const;

    void clearCache(const std::string& jwks_url);
    void clearAllCaches();

    /** Cache TTL in hours (default 24). */
    void setCacheTTL(int hours) { cache_ttl_hours_ = hours; }

    /** Minimum time between two fetches for an unknown kid (default 60 s; 0 = none). */
    void setRefetchCooldown(std::chrono::seconds cooldown) { refetch_cooldown_ = cooldown; }

    /** Replace how a JWKS document is fetched (tests). */
    void setFetcher(Fetcher fetcher) { fetcher_ = std::move(fetcher); }

    /** JWKS fetches attempted so far, failed ones included. */
    std::size_t fetchCount() const { return fetch_count_.load(); }

    /** Parse a JWKS document: RSA (n, e) and EC (crv, x, y) signature keys. */
    std::optional<std::vector<JWKSKey>> parseJWKSResponse(const std::string& json_content);

private:
    using KeySet = std::shared_ptr<const std::vector<JWKSKey>>;

    struct JWKSCache {
        KeySet keys;                                          // null until a fetch succeeds
        std::chrono::steady_clock::time_point refreshed_at;   // last successful fetch
        std::chrono::steady_clock::time_point attempted_at;   // last fetch, successful or not
        bool attempted = false;
        bool last_ok = false;
        std::uint64_t generation = 0;                         // bumped by every fetch
        std::unordered_map<std::string, std::chrono::steady_clock::time_point> unknown_kids;
        std::shared_ptr<std::mutex> fetch_mutex = std::make_shared<std::mutex>();
    };

    /** Fetch, parse and store (caller holds the entry's fetch_mutex). */
    bool fetchAndStore(const std::string& jwks_url);

    static std::shared_ptr<const JWKSKey> findKey(const KeySet& keys, const std::string& kid);
    bool fresh(const JWKSCache& entry, std::chrono::steady_clock::time_point now) const;
    std::chrono::steady_clock::duration retryDelay(const JWKSCache& entry) const;
    void rememberUnknown(JWKSCache& entry, const std::string& kid, std::chrono::steady_clock::time_point now);

    /**
     * Convert an RSA JWK (n, e) to EVP_PKEY for OpenSSL verification
     */
    EVP_PKEY* jwkToEVPKey(const crow::json::rvalue& jwk);

    /**
     * Convert an EC JWK (crv, x, y) to EVP_PKEY; nullptr unless the point is
     * on the named curve.
     */
    EVP_PKEY* ecJwkToEVPKey(const std::string& crv, const std::string& x_b64url, const std::string& y_b64url);

    /**
     * Base64URL decode helper
     */
    std::string base64urlDecode(const std::string& input);

    mutable std::mutex cache_mutex_;
    std::unordered_map<std::string, JWKSCache> jwks_cache_;
    std::atomic<int> cache_ttl_hours_{24};
    std::atomic<std::chrono::seconds> refetch_cooldown_{std::chrono::seconds(60)};
    Fetcher fetcher_;
    std::atomic<std::size_t> fetch_count_{0};
};

} // namespace flapi
