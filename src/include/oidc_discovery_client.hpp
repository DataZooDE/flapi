#pragma once

#include <string>
#include <vector>
#include <optional>
#include <memory>
#include <mutex>
#include <chrono>
#include <crow.h>

namespace flapi {

/**
 * OIDC Provider Metadata - Obtained from .well-known/openid-configuration
 */
struct OIDCProviderMetadata {
    std::string issuer;
    std::string authorization_endpoint;
    std::string token_endpoint;
    std::string userinfo_endpoint;
    std::string jwks_uri;
    std::string revocation_endpoint;
    std::string introspection_endpoint;
    std::vector<std::string> scopes_supported;
    std::vector<std::string> response_types_supported;
    std::vector<std::string> grant_types_supported;
    std::vector<std::string> token_endpoint_auth_methods_supported;
};

/**
 * OIDCDiscoveryClient fetches and caches OIDC provider metadata
 * from the .well-known/openid-configuration endpoint
 */
class OIDCDiscoveryClient {
public:
    OIDCDiscoveryClient();

    /**
     * Fetch OIDC provider metadata from discovery endpoint
     * @param issuer_url Base issuer URL (e.g., https://accounts.google.com)
     * @return Provider metadata or std::nullopt if fetch failed
     */
    std::optional<OIDCProviderMetadata> getProviderMetadata(const std::string& issuer_url);

    /**
     * Clear all cached metadata (for testing or forced refresh)
     */
    void clearCache();

    /**
     * Set cache TTL in seconds (default: 86400 = 24 hours)
     */
    void setCacheTTL(int ttl_seconds) { cache_ttl_seconds_ = ttl_seconds; }

private:
    struct CacheEntry {
        OIDCProviderMetadata metadata;
        std::chrono::steady_clock::time_point cached_at;
    };

    /**
     * Fetch discovery document from well-known endpoint
     */
    std::optional<OIDCProviderMetadata> fetchDiscoveryDocument(const std::string& issuer_url);

    /**
     * Parse JSON response from discovery endpoint
     */
    std::optional<OIDCProviderMetadata> parseDiscoveryResponse(const std::string& json_content);

    mutable std::mutex cache_mutex_;
    std::unordered_map<std::string, CacheEntry> metadata_cache_;
    int cache_ttl_seconds_ = 86400;  // 24 hours default
};

} // namespace flapi
