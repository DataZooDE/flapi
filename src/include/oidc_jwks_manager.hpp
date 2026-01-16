#pragma once

#include <string>
#include <vector>
#include <optional>
#include <memory>
#include <mutex>
#include <chrono>
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
    std::string kty;              // Key type (RSA)
    std::string use;              // Use (sig for signature)
    std::string alg;              // Algorithm (RS256, RS384, RS512)
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
          n(std::move(other.n)), e(std::move(other.e)),
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
            n = std::move(other.n);
            e = std::move(other.e);
            evp_key = other.evp_key;
            other.evp_key = nullptr;
        }
        return *this;
    }
};

/**
 * OIDCJWKSManager manages JWKS (JSON Web Key Set) fetching and caching
 * Handles key rotation and automatic refresh
 */
class OIDCJWKSManager {
public:
    OIDCJWKSManager();
    ~OIDCJWKSManager();

    /**
     * Fetch and cache JWKS from the provided URL
     * @param jwks_url URL to fetch JWKS from (e.g., from discovery endpoint)
     * @return true if successful, false otherwise
     */
    bool refreshJWKS(const std::string& jwks_url);

    /**
     * Get a specific key by kid (key ID)
     * Will attempt to refresh if key not found (for key rotation)
     * @param kid Key ID to lookup
     * @param jwks_url URL to refresh from if needed
     * @return Pointer to key or nullptr if not found
     */
    const JWKSKey* getKey(const std::string& kid, const std::string& jwks_url);

    /**
     * Check if JWKS cache needs refresh
     * @param jwks_url URL to check
     * @return true if needs refresh
     */
    bool needsRefresh(const std::string& jwks_url) const;

    /**
     * Clear cache for a specific URL
     */
    void clearCache(const std::string& jwks_url);

    /**
     * Clear all caches
     */
    void clearAllCaches();

    /**
     * Set cache TTL in hours (default: 24)
     */
    void setCacheTTL(int hours) { cache_ttl_hours_ = hours; }

private:
    struct JWKSCache {
        std::vector<JWKSKey> keys;
        std::chrono::steady_clock::time_point refreshed_at;
    };

    /**
     * Fetch JWKS from remote endpoint
     */
    std::optional<std::vector<JWKSKey>> fetchJWKS(const std::string& jwks_url);

    /**
     * Parse JWKS JSON response
     */
    std::optional<std::vector<JWKSKey>> parseJWKSResponse(const std::string& json_content);

    /**
     * Convert JWK to EVP_PKEY for OpenSSL verification
     */
    EVP_PKEY* jwkToEVPKey(const crow::json::rvalue& jwk);

    /**
     * Base64URL decode helper
     */
    std::string base64urlDecode(const std::string& input);

    mutable std::mutex cache_mutex_;
    std::unordered_map<std::string, JWKSCache> jwks_cache_;
    int cache_ttl_hours_ = 24;
};

} // namespace flapi
