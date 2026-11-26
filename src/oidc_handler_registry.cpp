#include "oidc_handler_registry.hpp"

#include <crow/logging.h>

#include "oidc_provider_presets.hpp"

namespace flapi {

namespace {

void append(std::string& key, const std::string& field) {
    key += std::to_string(field.size());
    key += ':';
    key += field;
    key += ';';
}

}  // namespace

OIDCHandlerRegistry& OIDCHandlerRegistry::instance() {
    static OIDCHandlerRegistry registry;
    return registry;
}

OIDCConfig OIDCHandlerRegistry::effectiveConfig(const OIDCConfig& config) {
    OIDCConfig effective = config;
    OIDCProviderPresets::applyPreset(effective);
    return effective;
}

std::string OIDCHandlerRegistry::keyFor(const OIDCConfig& c) {
    std::string key;
    append(key, c.provider_type);
    append(key, c.issuer_url);
    append(key, c.client_id);
    append(key, std::to_string(c.allowed_audiences.size()));
    for (const auto& audience : c.allowed_audiences) {
        append(key, audience);
    }
    append(key, c.verify_expiration ? "exp" : "no-exp");
    append(key, std::to_string(c.clock_skew_seconds));
    append(key, c.username_claim);
    append(key, c.email_claim);
    append(key, c.roles_claim);
    append(key, c.groups_claim);
    append(key, c.role_claim_path);
    append(key, std::to_string(c.jwks_cache_hours));
    append(key, std::to_string(c.jwks_refetch_cooldown_seconds));
    return key;
}

OIDCAuthHandler::Config OIDCHandlerRegistry::handlerConfigFor(const OIDCConfig& c) {
    OIDCAuthHandler::Config h;
    h.issuer_url = c.issuer_url;
    h.client_id = c.client_id;
    h.client_secret = c.client_secret;
    h.allowed_audiences = c.allowed_audiences;
    h.verify_expiration = c.verify_expiration;
    h.clock_skew_seconds = c.clock_skew_seconds;
    h.username_claim = c.username_claim;
    h.email_claim = c.email_claim;
    h.roles_claim = c.roles_claim;
    h.groups_claim = c.groups_claim;
    h.role_claim_path = c.role_claim_path;
    h.enable_client_credentials = c.enable_client_credentials;
    h.enable_refresh_tokens = c.enable_refresh_tokens;
    h.scopes = c.scopes;
    h.jwks_cache_hours = c.jwks_cache_hours;
    h.jwks_refetch_cooldown_seconds = c.jwks_refetch_cooldown_seconds;
    return h;
}

std::shared_ptr<OIDCAuthHandler> OIDCHandlerRegistry::handlerFor(const OIDCConfig& config) {
    // The preset is applied outside the lock: it only touches the copy.
    const OIDCConfig effective = effectiveConfig(config);
    const std::string key = keyFor(effective);

    std::lock_guard<std::mutex> lock(mutex_);
    if (auto it = handlers_.find(key); it != handlers_.end()) {
        return it->second;
    }
    // Keys are per issuer and per key-cache settings, shared across audiences
    // and claim mappings.
    std::string provider_key;
    append(provider_key, effective.issuer_url);
    append(provider_key, std::to_string(effective.jwks_cache_hours));
    append(provider_key, std::to_string(effective.jwks_refetch_cooldown_seconds));
    auto& provider = providers_[provider_key];
    if (!provider) {
        provider = std::make_shared<OIDCProviderKeys>(
            effective.issuer_url,
            OIDCProviderKeys::Options{effective.jwks_cache_hours, effective.jwks_refetch_cooldown_seconds});
    }
    auto handler = std::make_shared<OIDCAuthHandler>(handlerConfigFor(effective), provider);
    handlers_.emplace(key, handler);
    CROW_LOG_DEBUG << "OIDC handler registry: " << handlers_.size() << " handler(s), " << providers_.size()
                   << " provider(s)";
    return handler;
}

std::size_t OIDCHandlerRegistry::handlerCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return handlers_.size();
}

std::size_t OIDCHandlerRegistry::providerCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return providers_.size();
}

void OIDCHandlerRegistry::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    handlers_.clear();
    providers_.clear();
}

}  // namespace flapi
