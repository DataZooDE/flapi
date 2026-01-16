#include "include/oidc_discovery_client.hpp"
#include "include/http_client.hpp"
#include <crow/logging.h>
#include <sstream>

namespace flapi {

OIDCDiscoveryClient::OIDCDiscoveryClient() {}

std::optional<OIDCProviderMetadata> OIDCDiscoveryClient::getProviderMetadata(
    const std::string& issuer_url) {

    // Check cache first
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        auto it = metadata_cache_.find(issuer_url);
        if (it != metadata_cache_.end()) {
            auto age = std::chrono::steady_clock::now() - it->second.cached_at;
            if (age.count() < cache_ttl_seconds_ * 1000000000LL) {  // nanoseconds
                CROW_LOG_DEBUG << "Using cached OIDC metadata for: " << issuer_url;
                return it->second.metadata;
            }
        }
    }

    // Fetch fresh metadata
    auto metadata = fetchDiscoveryDocument(issuer_url);
    if (!metadata) {
        CROW_LOG_WARNING << "Failed to fetch OIDC discovery metadata from: " << issuer_url;
        return std::nullopt;
    }

    // Cache the result
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        metadata_cache_[issuer_url] = {
            *metadata,
            std::chrono::steady_clock::now()
        };
    }

    CROW_LOG_DEBUG << "Fetched and cached OIDC metadata for: " << issuer_url;
    return metadata;
}

std::optional<OIDCProviderMetadata> OIDCDiscoveryClient::fetchDiscoveryDocument(
    const std::string& issuer_url) {

    // Construct discovery endpoint URL
    std::string discovery_url = issuer_url;
    if (discovery_url.back() != '/') {
        discovery_url += '/';
    }
    discovery_url += ".well-known/openid-configuration";

    CROW_LOG_DEBUG << "Fetching OIDC discovery document from: " << discovery_url;

    // Fetch discovery document via HTTP
    auto response = HTTPClient::get(discovery_url);
    if (!response) {
        CROW_LOG_ERROR << "Failed to fetch OIDC discovery document from: " << discovery_url;
        return std::nullopt;
    }

    // Check HTTP status
    if (response->status_code != 200) {
        CROW_LOG_ERROR << "OIDC discovery endpoint returned status " << response->status_code
                       << " from: " << discovery_url;
        return std::nullopt;
    }

    // Parse JSON response
    auto metadata = parseDiscoveryResponse(response->body);
    if (!metadata) {
        CROW_LOG_ERROR << "Failed to parse OIDC discovery response from: " << discovery_url;
        return std::nullopt;
    }

    CROW_LOG_DEBUG << "Successfully fetched OIDC discovery metadata from: " << discovery_url;
    return metadata;
}

std::optional<OIDCProviderMetadata> OIDCDiscoveryClient::parseDiscoveryResponse(
    const std::string& json_content) {

    try {
        auto json = crow::json::load(json_content);

        OIDCProviderMetadata metadata;

        // Required fields
        if (!json.has("issuer")) {
            CROW_LOG_WARNING << "Discovery response missing 'issuer' field";
            return std::nullopt;
        }
        metadata.issuer = json["issuer"].s();

        if (!json.has("jwks_uri")) {
            CROW_LOG_WARNING << "Discovery response missing 'jwks_uri' field";
            return std::nullopt;
        }
        metadata.jwks_uri = json["jwks_uri"].s();

        // Optional endpoints
        if (json.has("authorization_endpoint")) {
            metadata.authorization_endpoint = json["authorization_endpoint"].s();
        }

        if (json.has("token_endpoint")) {
            metadata.token_endpoint = json["token_endpoint"].s();
        }

        if (json.has("userinfo_endpoint")) {
            metadata.userinfo_endpoint = json["userinfo_endpoint"].s();
        }

        if (json.has("revocation_endpoint")) {
            metadata.revocation_endpoint = json["revocation_endpoint"].s();
        }

        if (json.has("introspection_endpoint")) {
            metadata.introspection_endpoint = json["introspection_endpoint"].s();
        }

        // Scopes supported
        if (json.has("scopes_supported")) {
            auto scopes = json["scopes_supported"];
            if (scopes.t() == crow::json::type::List) {
                for (const auto& scope : scopes) {
                    metadata.scopes_supported.push_back(scope.s());
                }
            }
        }

        // Response types supported
        if (json.has("response_types_supported")) {
            auto types = json["response_types_supported"];
            if (types.t() == crow::json::type::List) {
                for (const auto& type : types) {
                    metadata.response_types_supported.push_back(type.s());
                }
            }
        }

        // Grant types supported
        if (json.has("grant_types_supported")) {
            auto types = json["grant_types_supported"];
            if (types.t() == crow::json::type::List) {
                for (const auto& type : types) {
                    metadata.grant_types_supported.push_back(type.s());
                }
            }
        }

        // Token endpoint auth methods
        if (json.has("token_endpoint_auth_methods_supported")) {
            auto methods = json["token_endpoint_auth_methods_supported"];
            if (methods.t() == crow::json::type::List) {
                for (const auto& method : methods) {
                    metadata.token_endpoint_auth_methods_supported.push_back(method.s());
                }
            }
        }

        CROW_LOG_DEBUG << "Parsed discovery document successfully";
        return metadata;

    } catch (const std::exception& e) {
        CROW_LOG_WARNING << "Error parsing discovery response: " << e.what();
        return std::nullopt;
    }
}

void OIDCDiscoveryClient::clearCache() {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    metadata_cache_.clear();
    CROW_LOG_DEBUG << "Cleared OIDC discovery cache";
}

} // namespace flapi
