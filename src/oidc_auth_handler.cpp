#include "include/oidc_auth_handler.hpp"
#include "include/http_client.hpp"
#include <crow/logging.h>
#include <openssl/evp.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/rsa.h>
#include <sstream>
#include <cmath>
#include <cstdint>
#include <ctime>

namespace flapi {

const JwsAlgorithm* FindJwsAlgorithm(const std::string& name) {
    // RS* and PS* need an RSA key, ES* an EC key on its curve. `none` and
    // HS* are not here: an OIDC token is signed by the issuer's private key,
    // and HS* with a public key as the "secret" is the classic forgery.
    static const JwsAlgorithm kAlgorithms[] = {
        {"RS256", EVP_sha256, false, nullptr, 0}, {"RS384", EVP_sha384, false, nullptr, 0},
        {"RS512", EVP_sha512, false, nullptr, 0}, {"PS256", EVP_sha256, true, nullptr, 0},
        {"PS384", EVP_sha384, true, nullptr, 0},  {"PS512", EVP_sha512, true, nullptr, 0},
        {"ES256", EVP_sha256, false, "P-256", 32}, {"ES384", EVP_sha384, false, "P-384", 48},
        {"ES512", EVP_sha512, false, "P-521", 66},
    };
    for (const auto& alg : kAlgorithms) {
        if (name == alg.name) {
            return &alg;
        }
    }
    return nullptr;
}

namespace {

// A JWT NumericDate as a time_point, clamped to [1970, 2255]: system_clock
// counts nanoseconds in 64 bits, so a larger value would overflow.
std::chrono::system_clock::time_point NumericDate(double seconds) {
    constexpr double kMax = 9.0e9;
    const double clamped = std::isnan(seconds) ? 0.0 : std::min(std::max(std::floor(seconds), 0.0), kMax);
    return std::chrono::system_clock::time_point(std::chrono::seconds(static_cast<std::int64_t>(clamped)));
}

// A failed discovery is not retried sooner than this.
constexpr auto kDiscoveryRetry = std::chrono::seconds(5);

}  // namespace

OIDCProviderKeys::OIDCProviderKeys(std::string issuer_url, const Options& options)
    : issuer_url_(std::move(issuer_url)) {
    if (options.jwks_cache_hours > 0) {
        jwks_.setCacheTTL(options.jwks_cache_hours);
    }
    jwks_.setRefetchCooldown(std::chrono::seconds(std::max(0, options.jwks_refetch_cooldown_seconds)));
}

std::optional<OIDCProviderMetadata> OIDCProviderKeys::metadata() {
    // Held across the fetch: concurrent first requests wait for this one.
    std::lock_guard<std::mutex> lock(metadata_mutex_);
    if (metadata_) {
        return metadata_;
    }
    const auto now = std::chrono::steady_clock::now();
    if (discovery_failed_ && now - discovery_failed_at_ < kDiscoveryRetry) {
        return std::nullopt;
    }
    ++discovery_count_;
    metadata_ = discovery_.getProviderMetadata(issuer_url_);
    discovery_failed_ = !metadata_;
    discovery_failed_at_ = now;
    if (metadata_ && metadata_->jwks_uri.empty()) {
        CROW_LOG_WARNING << "OIDC discovery document of " << issuer_url_ << " names no jwks_uri";
    }
    return metadata_;
}

std::shared_ptr<const JWKSKey> OIDCProviderKeys::key(const std::string& kid) {
    auto md = metadata();
    if (!md || md->jwks_uri.empty()) {
        return nullptr;
    }
    return jwks_.getKey(kid, md->jwks_uri);
}

bool OIDCProviderKeys::refreshJWKS() {
    auto md = metadata();
    return md && !md->jwks_uri.empty() && jwks_.refreshJWKS(md->jwks_uri);
}

OIDCAuthHandler::OIDCAuthHandler(const Config& config)
    : OIDCAuthHandler(config, std::make_shared<OIDCProviderKeys>(
                                  config.issuer_url,
                                  OIDCProviderKeys::Options{config.jwks_cache_hours,
                                                            config.jwks_refetch_cooldown_seconds})) {}

OIDCAuthHandler::OIDCAuthHandler(const Config& config, std::shared_ptr<OIDCProviderKeys> keys)
    : config_(config), keys_(std::move(keys)) {
    CROW_LOG_INFO << "Initialized OIDCAuthHandler for issuer: " << config_.issuer_url;
}

OIDCAuthHandler::~OIDCAuthHandler() {}

std::optional<OIDCTokenClaims> OIDCAuthHandler::validateToken(const std::string& token) {
    CROW_LOG_DEBUG << "Validating OIDC token";

    // Extract Bearer token if needed
    std::string actual_token = token;
    if (token.find("Bearer ") == 0) {
        actual_token = extractTokenFromHeader(token);
    }

    // Decode JWT header and payload (without verification first)
    auto decoded = decodeJWT(actual_token);
    if (!decoded) {
        logError("Failed to decode JWT");
        return std::nullopt;
    }

    // References into `decoded`, which outlives every use below. Do not copy
    // these into a value that outlives it (see DecodedJWT).
    const crow::json::rvalue& header = decoded->header();
    const crow::json::rvalue& payload = decoded->payload();

    // Extract kid (key ID) from header
    std::string kid;
    if (header.has("kid") && header["kid"].t() == crow::json::type::String) {
        kid = header["kid"].s();
    } else {
        logError("JWT header missing 'kid' field");
        return std::nullopt;
    }

    // The algorithm, before any key is looked up: `none`, HS256 or anything
    // else flAPI cannot verify is refused without touching the JWKS.
    std::string alg;
    if (header.has("alg") && header["alg"].t() == crow::json::type::String) {
        alg = header["alg"].s();
    } else {
        logError("JWT header missing 'alg' field");
        return std::nullopt;
    }
    if (FindJwsAlgorithm(alg) == nullptr) {
        logError("Unsupported signature algorithm: " + alg);
        return std::nullopt;
    }

    if (!keys_->metadata()) {
        logError("Failed to get provider metadata");
        return std::nullopt;
    }

    // The public key: at most one JWKS fetch for this request, and none
    // within the refetch cooldown (OIDCJWKSManager). Held by shared_ptr, so a
    // concurrent refresh cannot free it under us.
    const std::shared_ptr<const JWKSKey> key = keys_->key(kid);
    if (!key) {
        logError("Key not found in JWKS: " + kid);
        return std::nullopt;
    }

    // Verify JWT signature
    if (!verifySignature(actual_token, alg, key.get())) {
        logError("JWT signature verification failed");
        return std::nullopt;
    }

    CROW_LOG_DEBUG << "JWT signature verified successfully";

    // Extract claims
    OIDCTokenClaims claims;

    // Subject (required)
    auto sub = getClaimValue(payload, "sub");
    if (!sub) {
        logError("Token missing 'sub' claim");
        return std::nullopt;
    }
    claims.subject = *sub;

    // Username (mapped from configurable claim)
    auto username = getClaimValue(payload, config_.username_claim);
    claims.username = username ? *username : claims.subject;

    // Issuer (required for validation)
    auto iss = getClaimValue(payload, "iss");
    if (!iss) {
        logError("Token missing 'iss' claim");
        return std::nullopt;
    }
    claims.issuer = *iss;

    // Validate issuer matches
    if (claims.issuer != config_.issuer_url) {
        logError("Token issuer mismatch: " + claims.issuer + " != " + config_.issuer_url);
        return std::nullopt;
    }

    // Audience
    auto aud = getClaimArray(payload, "aud");
    if (aud) {
        claims.audience = *aud;
    }

    // Validate audience
    if (!validateAudience(claims)) {
        logError("Token audience validation failed");
        return std::nullopt;
    }

    // Expiration. A token without one never expires; refuse it rather than
    // lean on the zero time_point that an absent `exp` leaves behind.
    if (config_.verify_expiration && !payload.has("exp")) {
        logError("Token missing 'exp' claim");
        return std::nullopt;
    }
    if (payload.has("exp")) {
        if (payload["exp"].t() != crow::json::type::Number) {
            logError("Token 'exp' claim is not a number");
            return std::nullopt;
        }
        claims.expires_at = NumericDate(payload["exp"].d());
    }

    // Validate expiration
    if (config_.verify_expiration && isTokenExpired(claims)) {
        logError("Token has expired");
        return std::nullopt;
    }

    // Issued at, and not before: neither may lie in the future (beyond the
    // clock skew). Both used to be ignored.
    const auto skew = std::chrono::seconds(config_.clock_skew_seconds);
    const auto now = std::chrono::system_clock::now();
    for (const char* name : {"iat", "nbf"}) {
        if (!payload.has(name)) {
            continue;
        }
        if (payload[name].t() != crow::json::type::Number) {
            logError(std::string("Token '") + name + "' claim is not a number");
            return std::nullopt;
        }
        // d(), not i(): i() reads "1e+16" as 1, which would put a far-future
        // nbf written in exponent form in 1970.
        const auto at = NumericDate(payload[name].d());
        if (at > now + skew) {
            logError(std::string("Token '") + name + "' is in the future");
            return std::nullopt;
        }
        if (std::string(name) == "iat") {
            claims.issued_at = at;
        }
    }

    // JTI (JWT ID)
    auto jti = getClaimValue(payload, "jti");
    if (jti) {
        claims.jti = *jti;
    }

    // Email
    auto email = getClaimValue(payload, config_.email_claim);
    if (email) {
        claims.email = *email;
    }

    // Email verified
    if (payload.has("email_verified") &&
        (payload["email_verified"].t() == crow::json::type::True ||
         payload["email_verified"].t() == crow::json::type::False)) {
        claims.email_verified = payload["email_verified"].b();
    }

    // Name
    auto name = getClaimValue(payload, "name");
    if (name) {
        claims.name = *name;
    }

    // Roles (from configurable path)
    auto roles = getClaimArray(payload, config_.role_claim_path.empty() ?
                                          config_.roles_claim :
                                          config_.role_claim_path);
    if (roles) {
        claims.roles = *roles;
    }

    // Groups
    auto groups = getClaimArray(payload, config_.groups_claim);
    if (groups) {
        claims.groups = *groups;
    }

    CROW_LOG_INFO << "Token validated successfully for user: " << claims.username;
    return claims;
}

std::optional<OIDCProviderMetadata> OIDCAuthHandler::getProviderMetadata() {
    return keys_->metadata();
}

bool OIDCAuthHandler::isTokenExpired(const OIDCTokenClaims& claims) const {
    // system_clock, to match the epoch `exp` is expressed in.
    auto now = std::chrono::system_clock::now();
    auto skew = std::chrono::seconds(config_.clock_skew_seconds);
    return now > (claims.expires_at + skew);
}

bool OIDCAuthHandler::validateAudience(const OIDCTokenClaims& claims) const {
    // Without allowed-audiences the client id is the audience, as OIDC has it
    // for an ID token. "No audiences listed" used to mean "any audience", so
    // any token the issuer minted for any other application got in.
    std::vector<std::string> allowed_audiences = config_.allowed_audiences;
    if (allowed_audiences.empty()) {
        if (config_.client_id.empty()) {
            return true;   // generic provider with neither: nothing to check against
        }
        allowed_audiences.push_back(config_.client_id);
    }

    for (const auto& aud : claims.audience) {
        for (const auto& allowed : allowed_audiences) {
            if (aud == allowed) {
                return true;
            }
        }
    }

    logError("Token audience not in allowed list");
    return false;
}

bool OIDCAuthHandler::refreshJWKS() {
    return keys_->refreshJWKS();
}

std::string OIDCAuthHandler::extractTokenFromHeader(const std::string& auth_header) {
    const std::string bearer_prefix = "Bearer ";
    if (auth_header.find(bearer_prefix) == 0) {
        return auth_header.substr(bearer_prefix.length());
    }
    return auth_header;
}

std::optional<DecodedJWT> DecodedJWT::Decode(const std::string& token) {
    const size_t first_dot = token.find('.');
    if (first_dot == std::string::npos || first_dot == 0) {
        return std::nullopt;
    }
    const size_t second_dot = token.find('.', first_dot + 1);
    if (second_dot == std::string::npos || second_dot == first_dot + 1 ||
        token.find('.', second_dot + 1) != std::string::npos) {
        return std::nullopt;
    }

    // base64decode accepts the URL alphabet and missing padding.
    const std::string header_json =
        crow::utility::base64decode(token.substr(0, first_dot));
    const std::string payload_json =
        crow::utility::base64decode(token.substr(first_dot + 1, second_dot - first_dot - 1));

    // load() copies the text into a buffer the returned root owns. The roots
    // are MOVED into the result; a copy would not own that buffer.
    crow::json::rvalue header = crow::json::load(header_json);
    crow::json::rvalue payload = crow::json::load(payload_json);
    if (!header || header.t() != crow::json::type::Object ||
        !payload || payload.t() != crow::json::type::Object) {
        return std::nullopt;
    }
    return DecodedJWT(std::move(header), std::move(payload));
}

std::optional<DecodedJWT> OIDCAuthHandler::decodeJWT(const std::string& token) {
    try {
        auto decoded = DecodedJWT::Decode(token);
        if (!decoded) {
            logError("Invalid JWT format");
        }
        return decoded;
    } catch (const std::exception& e) {
        logError("JWT decode error: " + std::string(e.what()));
        return std::nullopt;
    }
}

std::optional<crow::json::rvalue> OIDCAuthHandler::findClaim(const crow::json::rvalue& object,
                                                             const std::string& claim_path) {
    if (object.t() != crow::json::type::Object || claim_path.empty()) {
        return std::nullopt;
    }
    // The name as written first: "https://example.com/roles" is ONE claim
    // (Auth0's namespaced custom claims), and splitting it at its dots found
    // nothing. Then "a.b.c" as a path, trying each dot from the left, so a
    // nested object whose own key contains a dot is found too.
    if (object.has(claim_path)) {
        return object[claim_path];
    }
    for (std::size_t dot = claim_path.find('.'); dot != std::string::npos; dot = claim_path.find('.', dot + 1)) {
        const std::string parent = claim_path.substr(0, dot);
        if (object.has(parent)) {
            auto found = findClaim(object[parent], claim_path.substr(dot + 1));
            if (found) {
                return found;
            }
        }
    }
    return std::nullopt;
}

std::optional<std::string> OIDCAuthHandler::getClaimValue(
    const crow::json::rvalue& payload,
    const std::string& claim_path) {

    try {
        auto value = findClaim(payload, claim_path);
        if (value && value->t() == crow::json::type::String) {
            return value->s();
        }
        return std::nullopt;
    } catch (const std::exception& e) {
        CROW_LOG_DEBUG << "Error extracting claim '" << claim_path << "': " << e.what();
        return std::nullopt;
    }
}

std::optional<std::vector<std::string>> OIDCAuthHandler::getClaimArray(
    const crow::json::rvalue& payload,
    const std::string& claim_path) {

    try {
        auto value = findClaim(payload, claim_path);
        if (!value) {
            return std::nullopt;
        }
        std::vector<std::string> result;
        if (value->t() == crow::json::type::List) {
            for (const auto& item : *value) {
                if (item.t() == crow::json::type::String) {
                    result.push_back(item.s());
                }
            }
            return result;
        }
        if (value->t() == crow::json::type::String) {
            // Single value treated as array of one
            result.push_back(value->s());
            return result;
        }
        return std::nullopt;
    } catch (const std::exception& e) {
        CROW_LOG_DEBUG << "Error extracting array claim '" << claim_path << "': " << e.what();
        return std::nullopt;
    }
}

bool OIDCAuthHandler::verifySignature(
    const std::string& token_to_verify,
    const std::string& algorithm,
    const JWKSKey* key) {

    if (!key || !key->evp_key) {
        logError("Invalid key for signature verification");
        return false;
    }
    const JwsAlgorithm* alg = FindJwsAlgorithm(algorithm);
    if (alg == nullptr) {
        logError("Unsupported signature algorithm: " + algorithm);
        return false;
    }
    // The key decides which algorithm may use it, not the token: a JWK that
    // names an `alg` verifies that one only, and every key only algorithms
    // of its own type (and, for EC, its own curve).
    if (!key->alg.empty() && key->alg != algorithm) {
        logError("Token alg " + algorithm + " does not match the key's alg " + key->alg + " (kid " + key->kid + ")");
        return false;
    }
    const int key_type = EVP_PKEY_base_id(key->evp_key);
    if (alg->ec_curve == nullptr ? key_type != EVP_PKEY_RSA
                                 : (key_type != EVP_PKEY_EC || key->crv != alg->ec_curve)) {
        logError("Token alg " + algorithm + " cannot be verified with key " + key->kid + " (" + key->kty +
                 (key->crv.empty() ? "" : " " + key->crv) + ")");
        return false;
    }

    const size_t second_dot = token_to_verify.rfind('.');
    if (second_dot == std::string::npos || second_dot == 0) {
        logError("Invalid token format for signature verification");
        return false;
    }
    const std::string header_payload = token_to_verify.substr(0, second_dot);
    std::string signature = crow::utility::base64decode(token_to_verify.substr(second_dot + 1));

    if (alg->ec_curve != nullptr) {
        // JWS carries an ECDSA signature as r || s, each the curve size;
        // OpenSSL verifies DER. A wrong length is refused, not padded.
        if (signature.size() != 2 * alg->ec_size) {
            logError("ECDSA signature has the wrong length for " + algorithm);
            return false;
        }
        ECDSA_SIG* sig = ECDSA_SIG_new();
        BIGNUM* r = BN_bin2bn(reinterpret_cast<const unsigned char*>(signature.data()),
                              static_cast<int>(alg->ec_size), nullptr);
        BIGNUM* s = BN_bin2bn(reinterpret_cast<const unsigned char*>(signature.data()) + alg->ec_size,
                              static_cast<int>(alg->ec_size), nullptr);
        if (!sig || !r || !s || ECDSA_SIG_set0(sig, r, s) != 1) {
            BN_free(r);
            BN_free(s);
            ECDSA_SIG_free(sig);
            logError("Failed to build the ECDSA signature");
            return false;
        }
        unsigned char* der = nullptr;
        const int der_len = i2d_ECDSA_SIG(sig, &der);
        ECDSA_SIG_free(sig);   // frees r and s
        if (der_len <= 0) {
            logError("Failed to encode the ECDSA signature");
            return false;
        }
        signature.assign(reinterpret_cast<const char*>(der), static_cast<size_t>(der_len));
        OPENSSL_free(der);
    }

    EVP_MD_CTX* mdctx = EVP_MD_CTX_new();
    if (!mdctx) {
        logError("Failed to create EVP_MD_CTX");
        return false;
    }
    EVP_PKEY_CTX* pctx = nullptr;
    bool ok = EVP_DigestVerifyInit(mdctx, &pctx, alg->md(), nullptr, key->evp_key) == 1;
    if (ok && alg->pss) {
        // RFC 7518 3.5: MGF1 with the same hash, salt as long as the hash.
        ok = EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING) == 1 &&
             EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, RSA_PSS_SALTLEN_DIGEST) == 1 &&
             EVP_PKEY_CTX_set_rsa_mgf1_md(pctx, alg->md()) == 1;
    } else if (ok && alg->ec_curve == nullptr) {
        ok = EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PADDING) == 1;
    }
    ok = ok && EVP_DigestVerify(mdctx, reinterpret_cast<const unsigned char*>(signature.data()), signature.size(),
                                reinterpret_cast<const unsigned char*>(header_payload.data()),
                                header_payload.size()) == 1;
    EVP_MD_CTX_free(mdctx);
    ERR_clear_error();
    if (!ok) {
        logError("Signature verification failed (" + algorithm + ", kid " + key->kid + ")");
        return false;
    }
    CROW_LOG_DEBUG << "Signature verification successful with algorithm: " << algorithm;
    return true;
}

void OIDCAuthHandler::logError(const std::string& error) const {
    CROW_LOG_WARNING << "OIDC error: " << error;
    if (error_callback_) {
        error_callback_(error);
    }
}

std::optional<OIDCAuthHandler::TokenResponse> OIDCAuthHandler::getClientCredentialsToken(
    const std::vector<std::string>& scopes) {

    CROW_LOG_DEBUG << "Getting access token using client credentials flow";

    // Ensure client credentials are configured
    if (config_.client_id.empty() || config_.client_secret.empty()) {
        logError("Client credentials not configured");
        return std::nullopt;
    }

    // Ensure client credentials flow is enabled
    if (!config_.enable_client_credentials) {
        logError("Client credentials flow is not enabled");
        return std::nullopt;
    }

    // Get provider metadata to find token endpoint
    auto metadata = getProviderMetadata();
    if (!metadata || metadata->token_endpoint.empty()) {
        logError("Cannot determine token endpoint for client credentials exchange");
        return std::nullopt;
    }

    // Use provided scopes or fall back to config scopes
    auto request_scopes = scopes.empty() ? config_.scopes : scopes;

    // Exchange credentials for token
    return exchangeClientCredentials(metadata->token_endpoint, request_scopes);
}

std::optional<OIDCAuthHandler::TokenResponse> OIDCAuthHandler::refreshAccessToken(
    const std::string& refresh_token,
    const std::vector<std::string>& scopes) {

    CROW_LOG_DEBUG << "Refreshing access token";

    if (refresh_token.empty()) {
        logError("Refresh token is empty");
        return std::nullopt;
    }

    if (!config_.enable_refresh_tokens) {
        logError("Refresh token flow is not enabled");
        return std::nullopt;
    }

    // Get provider metadata
    auto metadata = getProviderMetadata();
    if (!metadata || metadata->token_endpoint.empty()) {
        logError("Cannot determine token endpoint for token refresh");
        return std::nullopt;
    }

    // TODO: Phase 2 - Implement refresh token grant
    // For now, return nullopt to indicate not yet implemented
    CROW_LOG_INFO << "Token refresh not fully implemented yet (requires HTTP client)";
    return std::nullopt;
}

std::optional<OIDCAuthHandler::TokenResponse> OIDCAuthHandler::exchangeClientCredentials(
    const std::string& token_endpoint,
    const std::vector<std::string>& scopes) {

    CROW_LOG_DEBUG << "Exchanging client credentials at: " << token_endpoint;

    // Validate required credentials
    if (config_.client_id.empty() || config_.client_secret.empty()) {
        logError("Client credentials missing: client_id or client_secret");
        return std::nullopt;
    }

    // Build form data for token request
    std::ostringstream form_data_stream;
    form_data_stream << "grant_type=client_credentials";
    form_data_stream << "&client_id=" << config_.client_id;
    form_data_stream << "&client_secret=" << config_.client_secret;

    // Add scopes if provided
    std::vector<std::string> request_scopes = scopes;
    if (request_scopes.empty()) {
        request_scopes = config_.scopes;
    }

    if (!request_scopes.empty()) {
        std::ostringstream scope_stream;
        for (size_t i = 0; i < request_scopes.size(); ++i) {
            if (i > 0) scope_stream << " ";
            scope_stream << request_scopes[i];
        }
        form_data_stream << "&scope=" << scope_stream.str();
    }

    std::string form_data = form_data_stream.str();

    // Make HTTP POST request to token endpoint
    auto response = HTTPClient::post_form(token_endpoint, form_data);
    if (!response) {
        logError("Failed to POST to token endpoint: " + token_endpoint);
        return std::nullopt;
    }

    // Check HTTP status
    if (response->status_code != 200) {
        logError("Token endpoint returned status " + std::to_string(response->status_code));
        CROW_LOG_DEBUG << "Token endpoint response: " << response->body;
        return std::nullopt;
    }

    // Parse token response
    auto token_response = parseTokenResponse(response->body);
    if (!token_response) {
        logError("Failed to parse token endpoint response");
        return std::nullopt;
    }

    CROW_LOG_INFO << "Successfully obtained access token via client credentials flow";
    return token_response;
}

std::optional<OIDCAuthHandler::TokenResponse> OIDCAuthHandler::parseTokenResponse(
    const std::string& json_content) {

    try {
        auto json = crow::json::load(json_content);

        if (!json.has("access_token")) {
            logError("Token response missing 'access_token' field");
            return std::nullopt;
        }

        TokenResponse response;
        response.access_token = json["access_token"].s();

        // Optional fields
        if (json.has("token_type")) {
            response.token_type = json["token_type"].s();
        }

        if (json.has("expires_in")) {
            response.expires_in = json["expires_in"].i();
        }

        if (json.has("scope")) {
            response.scope = json["scope"].s();
        }

        if (json.has("refresh_token")) {
            response.refresh_token = json["refresh_token"].s();
        }

        CROW_LOG_DEBUG << "Token response parsed successfully";
        return response;

    } catch (const std::exception& e) {
        logError("Error parsing token response: " + std::string(e.what()));
        return std::nullopt;
    }
}

} // namespace flapi
