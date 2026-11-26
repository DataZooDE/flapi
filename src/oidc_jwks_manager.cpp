#include "include/oidc_jwks_manager.hpp"
#include "include/http_client.hpp"
#include <crow/logging.h>
#include <openssl/err.h>
#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/obj_mac.h>
#include <sstream>
#include <algorithm>

namespace flapi {

namespace {

// A failed fetch is retried sooner than a successful one is repeated: the
// issuer may only have blipped, and until it answers no token verifies.
constexpr auto kFailedFetchRetry = std::chrono::seconds(5);
// Bound on remembered unknown kids per JWKS URL; past it the list starts over.
constexpr std::size_t kMaxUnknownKids = 1024;

}  // namespace

OIDCJWKSManager::OIDCJWKSManager()
    : fetcher_([](const std::string& url) -> std::optional<std::string> {
          auto response = HTTPClient::get(url);
          if (!response) {
              CROW_LOG_ERROR << "Failed to fetch JWKS from: " << url;
              return std::nullopt;
          }
          if (response->status_code != 200) {
              CROW_LOG_ERROR << "JWKS endpoint returned status " << response->status_code << " from: " << url;
              return std::nullopt;
          }
          return response->body;
      }) {}

OIDCJWKSManager::~OIDCJWKSManager() {}

std::shared_ptr<const JWKSKey> OIDCJWKSManager::findKey(const KeySet& keys, const std::string& kid) {
    if (!keys) {
        return nullptr;
    }
    for (const auto& key : *keys) {
        if (key.kid == kid) {
            // Aliasing: shares ownership of the whole set, points at one key.
            return std::shared_ptr<const JWKSKey>(keys, &key);
        }
    }
    return nullptr;
}

bool OIDCJWKSManager::fresh(const JWKSCache& entry, std::chrono::steady_clock::time_point now) const {
    return entry.keys && now - entry.refreshed_at < std::chrono::hours(cache_ttl_hours_.load());
}

std::chrono::steady_clock::duration OIDCJWKSManager::retryDelay(const JWKSCache& entry) const {
    const std::chrono::steady_clock::duration cooldown = refetch_cooldown_.load();
    if (entry.last_ok) {
        return cooldown;
    }
    return std::min<std::chrono::steady_clock::duration>(cooldown, kFailedFetchRetry);
}

void OIDCJWKSManager::rememberUnknown(JWKSCache& entry, const std::string& kid,
                                      std::chrono::steady_clock::time_point now) {
    if (entry.unknown_kids.size() >= kMaxUnknownKids) {
        entry.unknown_kids.clear();
    }
    entry.unknown_kids[kid] = now;
}

bool OIDCJWKSManager::fetchAndStore(const std::string& jwks_url) {
    ++fetch_count_;
    CROW_LOG_DEBUG << "Fetching JWKS from: " << jwks_url;
    std::optional<std::vector<JWKSKey>> keys;
    if (auto body = fetcher_(jwks_url)) {
        keys = parseJWKSResponse(*body);
        if (!keys) {
            CROW_LOG_ERROR << "Failed to parse JWKS response from: " << jwks_url;
        }
    }
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto& entry = jwks_cache_[jwks_url];
    entry.attempted = true;
    entry.attempted_at = now;
    entry.last_ok = keys.has_value();
    ++entry.generation;
    if (!keys) {
        // The keys we had stay usable: a failed refetch does not log
        // everybody out.
        CROW_LOG_WARNING << "Failed to fetch JWKS from: " << jwks_url;
        return false;
    }
    CROW_LOG_DEBUG << "Fetched " << keys->size() << " keys from: " << jwks_url;
    entry.keys = std::make_shared<const std::vector<JWKSKey>>(std::move(*keys));
    entry.refreshed_at = now;
    entry.unknown_kids.clear();
    return true;
}

bool OIDCJWKSManager::refreshJWKS(const std::string& jwks_url) {
    std::shared_ptr<std::mutex> fetch_mutex;
    std::uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        auto& entry = jwks_cache_[jwks_url];
        fetch_mutex = entry.fetch_mutex;
        generation = entry.generation;
    }
    std::lock_guard<std::mutex> fetching(*fetch_mutex);
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        const auto& entry = jwks_cache_[jwks_url];
        if (entry.generation != generation) {
            return entry.last_ok;   // fetched while we waited
        }
    }
    return fetchAndStore(jwks_url);
}

std::shared_ptr<const JWKSKey> OIDCJWKSManager::getKey(const std::string& kid, const std::string& jwks_url) {
    const auto now = std::chrono::steady_clock::now();
    std::shared_ptr<std::mutex> fetch_mutex;
    std::uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        auto& entry = jwks_cache_[jwks_url];
        auto key = findKey(entry.keys, kid);
        if (key && fresh(entry, now)) {
            return key;
        }
        const auto unknown = entry.unknown_kids.find(kid);
        if (!key && unknown != entry.unknown_kids.end() && now - unknown->second < refetch_cooldown_.load()) {
            CROW_LOG_DEBUG << "JWKS kid known to be absent (negative cache): " << kid;
            return nullptr;
        }
        if (entry.attempted && now - entry.attempted_at < retryDelay(entry)) {
            // Within the cooldown: answer from what we have (a stale key
            // still verifies), never from the issuer.
            if (!key) {
                CROW_LOG_DEBUG << "JWKS kid not found, refetch cooldown active: " << kid;
                rememberUnknown(entry, kid, now);
            }
            return key;
        }
        fetch_mutex = entry.fetch_mutex;
        generation = entry.generation;
    }

    // One fetch at a time per URL; whoever waited uses its result.
    std::lock_guard<std::mutex> fetching(*fetch_mutex);
    bool fetched_by_other = false;
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        fetched_by_other = jwks_cache_[jwks_url].generation != generation;
    }
    if (!fetched_by_other) {
        CROW_LOG_DEBUG << "Fetching JWKS for kid: " << kid;
        fetchAndStore(jwks_url);
    }
    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto& entry = jwks_cache_[jwks_url];
    auto key = findKey(entry.keys, kid);
    if (!key) {
        CROW_LOG_WARNING << "Key not found in JWKS: " << kid;
        rememberUnknown(entry, kid, now);
    }
    return key;
}

bool OIDCJWKSManager::needsRefresh(const std::string& jwks_url) const {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto it = jwks_cache_.find(jwks_url);
    return it == jwks_cache_.end() || !fresh(it->second, std::chrono::steady_clock::now());
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

        // A string member of the JWK, or "" when absent or not a string.
        const auto text = [](const crow::json::rvalue& jwk, const char* name) -> std::string {
            if (!jwk.has(name) || jwk[name].t() != crow::json::type::String) {
                return {};
            }
            return jwk[name].s();
        };

        for (const auto& jwk : keys_array) {
            if (jwk.t() != crow::json::type::Object) {
                continue;
            }
            JWKSKey key;
            key.kid = text(jwk, "kid");
            if (key.kid.empty()) {
                CROW_LOG_DEBUG << "Skipping JWK without 'kid' field";
                continue;
            }
            key.kty = text(jwk, "kty");
            key.use = text(jwk, "use");
            // No default: a JWK without `alg` may verify any algorithm its key
            // type allows; one with `alg` only that one (verifySignature).
            key.alg = text(jwk, "alg");

            if (!key.use.empty() && key.use != "sig") {
                CROW_LOG_DEBUG << "Skipping JWK not meant for signatures: " << key.kid;
                continue;
            }

            if (key.kty == "RSA") {
                key.n = text(jwk, "n");
                key.e = text(jwk, "e");
                if (key.n.empty() || key.e.empty()) {
                    CROW_LOG_WARNING << "RSA key missing 'n' or 'e' field: " << key.kid;
                    continue;
                }
                key.evp_key = jwkToEVPKey(jwk);
            } else if (key.kty == "EC") {
                key.crv = text(jwk, "crv");
                key.evp_key = ecJwkToEVPKey(key.crv, text(jwk, "x"), text(jwk, "y"));
            } else {
                CROW_LOG_DEBUG << "Skipping JWK of unsupported type '" << key.kty << "': " << key.kid;
                continue;
            }
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

EVP_PKEY* OIDCJWKSManager::ecJwkToEVPKey(const std::string& crv, const std::string& x_b64url,
                                        const std::string& y_b64url) {
    int nid = NID_undef;
    std::size_t size = 0;
    if (crv == "P-256") {
        nid = NID_X9_62_prime256v1;
        size = 32;
    } else if (crv == "P-384") {
        nid = NID_secp384r1;
        size = 48;
    } else if (crv == "P-521") {
        nid = NID_secp521r1;
        size = 66;
    } else {
        CROW_LOG_WARNING << "EC JWK with unsupported curve '" << crv << "'";
        return nullptr;
    }
    const std::string x = base64urlDecode(x_b64url);
    const std::string y = base64urlDecode(y_b64url);
    if (x.size() != size || y.size() != size) {
        CROW_LOG_WARNING << "EC JWK coordinates do not have the size of curve " << crv;
        return nullptr;
    }

    EC_KEY* ec_key = EC_KEY_new_by_curve_name(nid);
    BIGNUM* bn_x = BN_bin2bn(reinterpret_cast<const unsigned char*>(x.data()), static_cast<int>(x.size()), nullptr);
    BIGNUM* bn_y = BN_bin2bn(reinterpret_cast<const unsigned char*>(y.data()), static_cast<int>(y.size()), nullptr);
    EVP_PKEY* pkey = nullptr;
    // set_public_key_affine_coordinates refuses a point that is not on the
    // curve (an invalid-curve attack needs one).
    if (ec_key && bn_x && bn_y && EC_KEY_set_public_key_affine_coordinates(ec_key, bn_x, bn_y) == 1 &&
        EC_KEY_check_key(ec_key) == 1) {
        pkey = EVP_PKEY_new();
        if (pkey && EVP_PKEY_assign_EC_KEY(pkey, ec_key) == 1) {
            ec_key = nullptr;   // owned by pkey now
        } else if (pkey) {
            EVP_PKEY_free(pkey);
            pkey = nullptr;
        }
    }
    BN_free(bn_x);
    BN_free(bn_y);
    if (ec_key) {
        EC_KEY_free(ec_key);
    }
    return pkey;
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
