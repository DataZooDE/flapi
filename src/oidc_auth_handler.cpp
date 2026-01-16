#include "include/oidc_auth_handler.hpp"
#include "include/http_client.hpp"
#include <crow/logging.h>
#include <jwt-cpp/jwt.h>
#include <openssl/evp.h>
#include <sstream>
#include <ctime>

namespace flapi {

OIDCAuthHandler::OIDCAuthHandler(const Config& config)
    : config_(config) {
    discovery_client_ = std::make_unique<OIDCDiscoveryClient>();
    jwks_manager_ = std::make_unique<OIDCJWKSManager>();

    if (config_.jwks_cache_hours > 0) {
        jwks_manager_->setCacheTTL(config_.jwks_cache_hours);
    }

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

    auto [header, payload] = *decoded;

    // Extract kid (key ID) from header
    std::string kid;
    if (header.has("kid")) {
        kid = header["kid"].s();
    } else {
        logError("JWT header missing 'kid' field");
        return std::nullopt;
    }

    // Get provider metadata if not cached
    if (!cached_metadata_) {
        cached_metadata_ = getProviderMetadata();
        if (!cached_metadata_) {
            logError("Failed to get provider metadata");
            return std::nullopt;
        }
    }

    // Ensure JWKS is loaded
    if (!jwks_manager_->getKey(kid, cached_metadata_->jwks_uri)) {
        if (!refreshJWKS()) {
            logError("Failed to load JWKS for token validation");
            return std::nullopt;
        }
    }

    // Get the public key
    const JWKSKey* key = jwks_manager_->getKey(kid, cached_metadata_->jwks_uri);
    if (!key) {
        logError("Key not found in JWKS: " + kid);
        return std::nullopt;
    }

    // Get algorithm from header
    std::string alg;
    if (header.has("alg")) {
        alg = header["alg"].s();
    } else {
        logError("JWT header missing 'alg' field");
        return std::nullopt;
    }

    // Verify JWT signature
    if (!verifySignature(actual_token, alg, key)) {
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

    // Expiration
    if (payload.has("exp")) {
        try {
            uint64_t exp_seconds = (uint64_t)payload["exp"].i();
            claims.expires_at = std::chrono::steady_clock::time_point(
                std::chrono::seconds(exp_seconds)
            );
        } catch (...) {
            logError("Failed to parse 'exp' claim");
            return std::nullopt;
        }
    }

    // Validate expiration
    if (config_.verify_expiration && isTokenExpired(claims)) {
        logError("Token has expired");
        return std::nullopt;
    }

    // Issued at
    if (payload.has("iat")) {
        try {
            uint64_t iat_seconds = (uint64_t)payload["iat"].i();
            claims.issued_at = std::chrono::steady_clock::time_point(
                std::chrono::seconds(iat_seconds)
            );
        } catch (...) {
            CROW_LOG_DEBUG << "Failed to parse 'iat' claim";
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
    if (payload.has("email_verified")) {
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
    return discovery_client_->getProviderMetadata(config_.issuer_url);
}

bool OIDCAuthHandler::isTokenExpired(const OIDCTokenClaims& claims) const {
    auto now = std::chrono::steady_clock::now();
    auto skew = std::chrono::seconds(config_.clock_skew_seconds);
    return now > (claims.expires_at + skew);
}

bool OIDCAuthHandler::validateAudience(const OIDCTokenClaims& claims) const {
    if (config_.allowed_audiences.empty()) {
        // No audience restrictions
        return true;
    }

    for (const auto& aud : claims.audience) {
        for (const auto& allowed : config_.allowed_audiences) {
            if (aud == allowed) {
                return true;
            }
        }
    }

    logError("Token audience not in allowed list");
    return false;
}

bool OIDCAuthHandler::refreshJWKS() {
    if (!cached_metadata_) {
        cached_metadata_ = getProviderMetadata();
        if (!cached_metadata_) {
            return false;
        }
    }

    return jwks_manager_->refreshJWKS(cached_metadata_->jwks_uri);
}

std::string OIDCAuthHandler::extractTokenFromHeader(const std::string& auth_header) {
    const std::string bearer_prefix = "Bearer ";
    if (auth_header.find(bearer_prefix) == 0) {
        return auth_header.substr(bearer_prefix.length());
    }
    return auth_header;
}

std::optional<std::pair<crow::json::rvalue, crow::json::rvalue>>
OIDCAuthHandler::decodeJWT(const std::string& token) {
    try {
        // Use jwt-cpp to decode without verification
        auto decoded = jwt::decode(token);

        // Get header and payload as JSON
        // Extract the JSON parts from the token manually
        size_t first_dot = token.find('.');
        size_t second_dot = token.find('.', first_dot + 1);

        if (first_dot == std::string::npos || second_dot == std::string::npos) {
            logError("Invalid JWT format");
            return std::nullopt;
        }

        std::string header_b64 = token.substr(0, first_dot);
        std::string payload_b64 = token.substr(first_dot + 1, second_dot - first_dot - 1);

        // Decode base64
        std::string header_json = crow::utility::base64decode(header_b64);
        std::string payload_json = crow::utility::base64decode(payload_b64);

        // Parse JSON
        auto header = crow::json::load(header_json);
        auto payload = crow::json::load(payload_json);

        return std::make_pair(header, payload);

    } catch (const std::exception& e) {
        logError("JWT decode error: " + std::string(e.what()));
        return std::nullopt;
    }
}

std::optional<std::string> OIDCAuthHandler::getClaimValue(
    const crow::json::rvalue& payload,
    const std::string& claim_path) {

    try {
        // Handle nested claims (e.g., "realm_access.roles")
        size_t dot_pos = claim_path.find('.');
        if (dot_pos != std::string::npos) {
            std::string parent = claim_path.substr(0, dot_pos);
            std::string child = claim_path.substr(dot_pos + 1);

            if (!payload.has(parent)) {
                return std::nullopt;
            }

            auto parent_obj = payload[parent.c_str()];
            return getClaimValue(parent_obj, child);
        }

        // Simple claim lookup
        if (!payload.has(claim_path.c_str())) {
            return std::nullopt;
        }

        auto value = payload[claim_path.c_str()];
        if (value.t() == crow::json::type::String) {
            return value.s();
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
        // Handle nested claims
        size_t dot_pos = claim_path.find('.');
        if (dot_pos != std::string::npos) {
            std::string parent = claim_path.substr(0, dot_pos);
            std::string child = claim_path.substr(dot_pos + 1);

            if (!payload.has(parent.c_str())) {
                return std::nullopt;
            }

            auto parent_obj = payload[parent.c_str()];
            return getClaimArray(parent_obj, child);
        }

        if (!payload.has(claim_path.c_str())) {
            return std::nullopt;
        }

        auto value = payload[claim_path.c_str()];
        std::vector<std::string> result;

        if (value.t() == crow::json::type::List) {
            for (const auto& item : value) {
                if (item.t() == crow::json::type::String) {
                    result.push_back(item.s());
                }
            }
            return result;
        } else if (value.t() == crow::json::type::String) {
            // Single value treated as array of one
            result.push_back(value.s());
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

    try {
        // Determine hash function based on algorithm
        const EVP_MD* md = nullptr;
        if (algorithm == "RS256") {
            md = EVP_sha256();
        } else if (algorithm == "RS384") {
            md = EVP_sha384();
        } else if (algorithm == "RS512") {
            md = EVP_sha512();
        } else {
            logError("Unsupported signature algorithm: " + algorithm);
            return false;
        }

        if (!md) {
            logError("Failed to get message digest for: " + algorithm);
            return false;
        }

        // Split token into parts
        size_t first_dot = token_to_verify.find('.');
        size_t second_dot = token_to_verify.find('.', first_dot + 1);

        if (first_dot == std::string::npos || second_dot == std::string::npos) {
            logError("Invalid token format for signature verification");
            return false;
        }

        std::string header_payload = token_to_verify.substr(0, second_dot);
        std::string signature_b64 = token_to_verify.substr(second_dot + 1);

        // Decode signature
        std::string signature = crow::utility::base64decode(signature_b64);

        // Create EVP context
        EVP_MD_CTX* mdctx = EVP_MD_CTX_new();
        if (!mdctx) {
            logError("Failed to create EVP_MD_CTX");
            return false;
        }

        // Verify signature
        int result = EVP_VerifyInit_ex(mdctx, md, nullptr);
        if (result != 1) {
            EVP_MD_CTX_free(mdctx);
            logError("EVP_VerifyInit failed");
            return false;
        }

        result = EVP_VerifyUpdate(mdctx, header_payload.data(), header_payload.size());
        if (result != 1) {
            EVP_MD_CTX_free(mdctx);
            logError("EVP_VerifyUpdate failed");
            return false;
        }

        result = EVP_VerifyFinal(
            mdctx,
            reinterpret_cast<const unsigned char*>(signature.data()),
            signature.size(),
            key->evp_key
        );

        EVP_MD_CTX_free(mdctx);

        if (result != 1) {
            logError("EVP_VerifyFinal failed - signature verification error");
            return false;
        }

        CROW_LOG_DEBUG << "Signature verification successful with algorithm: " << algorithm;
        return true;

    } catch (const std::exception& e) {
        logError("Exception during signature verification: " + std::string(e.what()));
        return false;
    }
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
