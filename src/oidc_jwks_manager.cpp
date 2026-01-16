#include "include/oidc_jwks_manager.hpp"
#include "include/http_client.hpp"
#include <crow/logging.h>
#include <openssl/err.h>
#include <openssl/bn.h>
#include <sstream>
#include <algorithm>

namespace flapi {

OIDCJWKSManager::OIDCJWKSManager() {}

OIDCJWKSManager::~OIDCJWKSManager() {}

bool OIDCJWKSManager::refreshJWKS(const std::string& jwks_url) {
    auto keys = fetchJWKS(jwks_url);
    if (!keys) {
        CROW_LOG_WARNING << "Failed to fetch JWKS from: " << jwks_url;
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        jwks_cache_[jwks_url] = {
            std::move(*keys),
            std::chrono::steady_clock::now()
        };
    }

    CROW_LOG_DEBUG << "Successfully refreshed JWKS from: " << jwks_url;
    return true;
}

const JWKSKey* OIDCJWKSManager::getKey(
    const std::string& kid,
    const std::string& jwks_url) {

    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        auto it = jwks_cache_.find(jwks_url);
        if (it != jwks_cache_.end()) {
            // Check if cache is still valid
            auto age = std::chrono::steady_clock::now() - it->second.refreshed_at;
            if (age.count() < cache_ttl_hours_ * 3600LL * 1000000000LL) {  // nanoseconds
                // Search for key in cache
                for (auto& key : it->second.keys) {
                    if (key.kid == kid) {
                        CROW_LOG_DEBUG << "Found key in JWKS cache: " << kid;
                        return &key;
                    }
                }
            }
        }
    }

    // Key not found in cache, try to refresh (for key rotation)
    CROW_LOG_DEBUG << "Key not found in cache, attempting refresh: " << kid;
    if (!refreshJWKS(jwks_url)) {
        CROW_LOG_WARNING << "Failed to refresh JWKS, key not found: " << kid;
        return nullptr;
    }

    // Try again after refresh
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        auto it = jwks_cache_.find(jwks_url);
        if (it != jwks_cache_.end()) {
            for (auto& key : it->second.keys) {
                if (key.kid == kid) {
                    CROW_LOG_DEBUG << "Found key after JWKS refresh: " << kid;
                    return &key;
                }
            }
        }
    }

    CROW_LOG_WARNING << "Key not found in JWKS even after refresh: " << kid;
    return nullptr;
}

bool OIDCJWKSManager::needsRefresh(const std::string& jwks_url) const {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto it = jwks_cache_.find(jwks_url);
    if (it == jwks_cache_.end()) {
        return true;  // No cache entry, needs refresh
    }

    auto age = std::chrono::steady_clock::now() - it->second.refreshed_at;
    return age.count() >= cache_ttl_hours_ * 3600LL * 1000000000LL;  // nanoseconds
}

void OIDCJWKSManager::clearCache(const std::string& jwks_url) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    jwks_cache_.erase(jwks_url);
    CROW_LOG_DEBUG << "Cleared JWKS cache for: " << jwks_url;
}

void OIDCJWKSManager::clearAllCaches() {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    jwks_cache_.clear();
    CROW_LOG_DEBUG << "Cleared all JWKS caches";
}

std::optional<std::vector<JWKSKey>> OIDCJWKSManager::fetchJWKS(
    const std::string& jwks_url) {

    CROW_LOG_DEBUG << "Fetching JWKS from: " << jwks_url;

    // Fetch JWKS via HTTP GET
    auto response = HTTPClient::get(jwks_url);
    if (!response) {
        CROW_LOG_ERROR << "Failed to fetch JWKS from: " << jwks_url;
        return std::nullopt;
    }

    // Check HTTP status
    if (response->status_code != 200) {
        CROW_LOG_ERROR << "JWKS endpoint returned status " << response->status_code
                       << " from: " << jwks_url;
        return std::nullopt;
    }

    // Parse JSON response
    auto keys = parseJWKSResponse(response->body);
    if (!keys) {
        CROW_LOG_ERROR << "Failed to parse JWKS response from: " << jwks_url;
        return std::nullopt;
    }

    CROW_LOG_DEBUG << "Successfully fetched " << keys->size() << " keys from: " << jwks_url;
    return keys;
}

std::optional<std::vector<JWKSKey>> OIDCJWKSManager::parseJWKSResponse(
    const std::string& json_content) {

    try {
        auto json = crow::json::load(json_content);

        if (!json.has("keys")) {
            CROW_LOG_WARNING << "JWKS response missing 'keys' field";
            return std::nullopt;
        }

        auto keys_array = json["keys"];
        if (keys_array.t() != crow::json::type::List) {
            CROW_LOG_WARNING << "JWKS 'keys' field is not an array";
            return std::nullopt;
        }

        std::vector<JWKSKey> keys;

        for (const auto& jwk : keys_array) {
            if (!jwk.has("kid")) {
                CROW_LOG_DEBUG << "Skipping JWK without 'kid' field";
                continue;
            }

            JWKSKey key;
            key.kid = jwk["kid"].s();
            key.kty = jwk.has("kty") ? std::string(jwk["kty"].s()) : "RSA";

            // Only process RSA keys for signature verification
            if (key.kty != "RSA") {
                CROW_LOG_DEBUG << "Skipping non-RSA key: " << key.kid;
                continue;
            }

            key.use = jwk.has("use") ? std::string(jwk["use"].s()) : "sig";
            key.alg = jwk.has("alg") ? std::string(jwk["alg"].s()) : "RS256";

            // Extract RSA modulus and exponent
            if (!jwk.has("n") || !jwk.has("e")) {
                CROW_LOG_WARNING << "RSA key missing 'n' or 'e' field: " << key.kid;
                continue;
            }

            key.n = jwk["n"].s();
            key.e = jwk["e"].s();

            // Convert JWK to EVP_PKEY for OpenSSL verification
            key.evp_key = jwkToEVPKey(jwk);
            if (!key.evp_key) {
                CROW_LOG_WARNING << "Failed to convert JWK to EVP_PKEY: " << key.kid;
                continue;
            }

            keys.push_back(std::move(key));
        }

        CROW_LOG_DEBUG << "Parsed JWKS with " << keys.size() << " keys";
        return keys;

    } catch (const std::exception& e) {
        CROW_LOG_WARNING << "Error parsing JWKS response: " << e.what();
        return std::nullopt;
    }
}

EVP_PKEY* OIDCJWKSManager::jwkToEVPKey(const crow::json::rvalue& jwk) {
    try {
        if (!jwk.has("n") || !jwk.has("e")) {
            CROW_LOG_WARNING << "JWK missing RSA components";
            return nullptr;
        }

        std::string n_b64url = jwk["n"].s();
        std::string e_b64url = jwk["e"].s();

        // Decode base64url
        std::string n_bytes = base64urlDecode(n_b64url);
        std::string e_bytes = base64urlDecode(e_b64url);

        if (n_bytes.empty() || e_bytes.empty()) {
            CROW_LOG_WARNING << "Failed to decode RSA components";
            return nullptr;
        }

        // Create BIGNUM from bytes
        BIGNUM* bn_n = BN_bin2bn(
            reinterpret_cast<const unsigned char*>(n_bytes.data()),
            n_bytes.size(),
            nullptr
        );
        BIGNUM* bn_e = BN_bin2bn(
            reinterpret_cast<const unsigned char*>(e_bytes.data()),
            e_bytes.size(),
            nullptr
        );

        if (!bn_n || !bn_e) {
            CROW_LOG_WARNING << "Failed to create BIGNUMs for RSA key";
            if (bn_n) BN_free(bn_n);
            if (bn_e) BN_free(bn_e);
            return nullptr;
        }

        // Create RSA key structure
        RSA* rsa = RSA_new();
        if (!rsa) {
            CROW_LOG_WARNING << "Failed to allocate RSA structure";
            BN_free(bn_n);
            BN_free(bn_e);
            return nullptr;
        }

        // Set RSA components (ownership transferred to RSA)
        if (!RSA_set0_key(rsa, bn_n, bn_e, nullptr)) {
            CROW_LOG_WARNING << "Failed to set RSA key components";
            RSA_free(rsa);
            return nullptr;
        }

        // Create EVP_PKEY from RSA
        EVP_PKEY* pkey = EVP_PKEY_new();
        if (!pkey) {
            CROW_LOG_WARNING << "Failed to allocate EVP_PKEY";
            RSA_free(rsa);
            return nullptr;
        }

        if (!EVP_PKEY_set1_RSA(pkey, rsa)) {
            CROW_LOG_WARNING << "Failed to set RSA in EVP_PKEY";
            EVP_PKEY_free(pkey);
            RSA_free(rsa);
            return nullptr;
        }

        // EVP_PKEY_set1_RSA increments reference count, so we can free rsa
        RSA_free(rsa);

        return pkey;

    } catch (const std::exception& e) {
        CROW_LOG_WARNING << "Exception converting JWK to EVP_PKEY: " << e.what();
        return nullptr;
    }
}

std::string OIDCJWKSManager::base64urlDecode(const std::string& input) {
    std::string output = input;

    // Convert base64url to standard base64
    // Replace - with + and _ with /
    for (auto& c : output) {
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
    }

    // Add padding if necessary
    switch (output.size() % 4) {
        case 1: output += "==="; break;
        case 2: output += "=="; break;
        case 3: output += "="; break;
    }

    // Use Crow's base64 decoder
    try {
        return crow::utility::base64decode(output);
    } catch (const std::exception& e) {
        CROW_LOG_DEBUG << "Base64 decode error: " << e.what();
        return "";
    }
}

} // namespace flapi
