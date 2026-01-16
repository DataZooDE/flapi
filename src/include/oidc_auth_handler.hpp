#pragma once

#include <string>
#include <vector>
#include <optional>
#include <memory>
#include <chrono>
#include <crow.h>
#include "oidc_discovery_client.hpp"
#include "oidc_jwks_manager.hpp"

namespace flapi {

/**
 * OIDC token claims extracted from validated JWT
 */
struct OIDCTokenClaims {
    std::string subject;              // 'sub' claim - user ID
    std::string issuer;               // 'iss' claim
    std::vector<std::string> audience; // 'aud' claim(s)
    std::string username;             // Mapped from configurable claim (default: sub)
    std::string email;                // 'email' claim if available
    bool email_verified = false;      // 'email_verified' claim
    std::string name;                 // 'name' claim if available
    std::vector<std::string> roles;   // Extracted from roles/groups claims
    std::vector<std::string> groups;  // Group memberships
    std::chrono::steady_clock::time_point issued_at;  // 'iat' claim
    std::chrono::steady_clock::time_point expires_at; // 'exp' claim
    std::string jti;                  // JWT ID - unique token identifier
};

/**
 * OIDCAuthHandler validates OIDC/OAuth 2.0 tokens
 * Supports RSA signature verification and claims validation
 */
class OIDCAuthHandler {
public:
    struct Config {
        std::string issuer_url;
        std::string client_id;
        std::string client_secret;
        std::vector<std::string> allowed_audiences;
        bool verify_expiration = true;
        int clock_skew_seconds = 300;  // 5 minute tolerance
        std::string username_claim = "sub";
        std::string email_claim = "email";
        std::string roles_claim = "roles";
        std::string groups_claim = "groups";
        std::string role_claim_path;  // For nested claims like "realm_access.roles"
        bool enable_client_credentials = false;
        bool enable_refresh_tokens = false;
        std::vector<std::string> scopes;
        int jwks_cache_hours = 24;
    };

    explicit OIDCAuthHandler(const Config& config);
    ~OIDCAuthHandler();

    /**
     * Validate an OIDC token (JWT)
     * @param token Authorization header value or raw token
     * @return Token claims or std::nullopt if validation failed
     */
    std::optional<OIDCTokenClaims> validateToken(const std::string& token);

    /**
     * Get provider metadata (triggers discovery if needed)
     * @return Provider metadata or std::nullopt if discovery failed
     */
    std::optional<OIDCProviderMetadata> getProviderMetadata();

    /**
     * Check if a token is expired (considering clock skew)
     * @param claims Token claims
     * @return true if token is expired
     */
    bool isTokenExpired(const OIDCTokenClaims& claims) const;

    /**
     * Check if a token's audience matches allowed audiences
     * @param claims Token claims
     * @return true if audience is valid
     */
    bool validateAudience(const OIDCTokenClaims& claims) const;

    /**
     * Refresh JWKS from provider (for key rotation handling)
     * @return true if successful
     */
    bool refreshJWKS();

    /**
     * Set custom error handler for logging/debugging
     */
    using ErrorCallback = std::function<void(const std::string&)>;
    void setErrorCallback(ErrorCallback callback) {
        error_callback_ = callback;
    }

    /**
     * OAuth 2.0 Client Credentials Flow
     * Get an access token for service-to-service authentication
     * @param scopes Optional scopes to request (uses config.scopes if empty)
     * @return Token response with access_token and metadata, or nullopt if failed
     */
    struct TokenResponse {
        std::string access_token;
        std::string token_type = "Bearer";  // Usually "Bearer"
        int expires_in = 3600;              // Seconds until expiration
        std::string scope;                  // Granted scopes
        std::string refresh_token;          // Optional, if server supports refresh
    };

    std::optional<TokenResponse> getClientCredentialsToken(
        const std::vector<std::string>& scopes = {}
    );

    /**
     * Refresh an access token using a refresh token
     * @param refresh_token Refresh token from previous token response
     * @param scopes Optional scopes to request
     * @return New token response or nullopt if failed
     */
    std::optional<TokenResponse> refreshAccessToken(
        const std::string& refresh_token,
        const std::vector<std::string>& scopes = {}
    );

private:
    Config config_;
    std::unique_ptr<OIDCDiscoveryClient> discovery_client_;
    std::unique_ptr<OIDCJWKSManager> jwks_manager_;
    std::optional<OIDCProviderMetadata> cached_metadata_;
    ErrorCallback error_callback_;

    /**
     * Extract "Bearer " prefix from Authorization header
     */
    std::string extractTokenFromHeader(const std::string& auth_header);

    /**
     * Decode JWT without verification (to extract kid and claims)
     */
    std::optional<std::pair<crow::json::rvalue, crow::json::rvalue>>
    decodeJWT(const std::string& token);

    /**
     * Extract claim value from token payload (supports nested paths)
     */
    std::optional<std::string> getClaimValue(
        const crow::json::rvalue& payload,
        const std::string& claim_path
    );

    /**
     * Extract array claim from payload
     */
    std::optional<std::vector<std::string>> getClaimArray(
        const crow::json::rvalue& payload,
        const std::string& claim_path
    );

    /**
     * Validate JWT signature using public key from JWKS
     */
    bool verifySignature(
        const std::string& token_to_verify,
        const std::string& algorithm,
        const JWKSKey* key
    );

    /**
     * Log error if callback is set
     */
    void logError(const std::string& error) const;

    /**
     * Exchange client credentials for access token at token endpoint
     * Phase 2: Will implement HTTP request to token_endpoint
     */
    std::optional<TokenResponse> exchangeClientCredentials(
        const std::string& token_endpoint,
        const std::vector<std::string>& scopes
    );

    /**
     * Parse token endpoint response
     */
    std::optional<TokenResponse> parseTokenResponse(const std::string& json_content);
};

} // namespace flapi
