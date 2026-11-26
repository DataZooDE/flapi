#include "include/oidc_provider_presets.hpp"
#include <crow/logging.h>
#include <sstream>

namespace flapi {

namespace {

void replaceAll(std::string& text, const std::string& from, const std::string& to) {
    if (to.empty()) {
        return;
    }
    std::size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::string::npos) {
        text.replace(pos, from.size(), to);
        pos += to.size();
    }
}

// GitHub OAuth apps hand out opaque access tokens, not OIDC ID tokens: there
// is no discovery document or JWKS at https://github.com, so the preset could
// never validate a token. GitHub Actions does issue OIDC tokens, from its own
// issuer, and those work with the generic provider.
constexpr const char* kGitHubUnsupported =
    "provider-type: github is not supported: GitHub OAuth apps issue no OIDC tokens flAPI could "
    "validate. For GitHub Actions OIDC tokens use provider-type: generic with "
    "issuer-url: https://token.actions.githubusercontent.com and allowed-audiences.";

}  // namespace

std::string OIDCProviderPresets::normalizeProviderType(const std::string& provider_type) {
    // The docs call Microsoft Entra ID / Azure AD `azure`; the code called it
    // `microsoft`, and `azure` fell through as an unknown provider.
    if (provider_type == "azure" || provider_type == "azure-ad" || provider_type == "entra") {
        return "microsoft";
    }
    return provider_type;
}

void OIDCProviderPresets::substitutePlaceholders(OIDCConfig& config) {
    replaceAll(config.issuer_url, "{tenant}", config.tenant_id);
    replaceAll(config.issuer_url, "{tenant-id}", config.tenant_id);
    replaceAll(config.issuer_url, "{realm}", config.realm);
    replaceAll(config.issuer_url, "{domain}", config.domain);
}

bool OIDCProviderPresets::applyPreset(OIDCConfig& config) {
    config.provider_type = normalizeProviderType(config.provider_type);

    bool recognised = true;
    if (config.provider_type.empty() || config.provider_type == "generic") {
        // Generic OIDC: issuer_url must be provided
        recognised = false;
    } else if (config.provider_type == "google") {
        applyGooglePreset(config);
    } else if (config.provider_type == "microsoft") {
        applyMicrosoftPreset(config);
    } else if (config.provider_type == "keycloak") {
        applyKeycloakPreset(config);
    } else if (config.provider_type == "auth0") {
        applyAuth0Preset(config);
    } else if (config.provider_type == "okta") {
        applyOktaPreset(config);
    } else if (config.provider_type == "github") {
        // Recognised, and refused by validateProviderConfig: GitHub OAuth
        // apps issue no OIDC ID tokens to validate.
    } else {
        CROW_LOG_WARNING << "Unknown OIDC provider type: " << config.provider_type;
        recognised = false;
    }

    // After the preset: it may have supplied the issuer-url template whose
    // placeholders these fill. Nothing substituted them before, so a preset
    // issuer-url was never a usable URL.
    substitutePlaceholders(config);

    // Auth0 issues `iss` with a trailing slash ("https://acme.auth0.com/"),
    // and the issuer comparison is exact: without the slash every Auth0 token
    // was refused.
    if (config.provider_type == "auth0" && !config.issuer_url.empty() && !hasPlaceholders(config.issuer_url) &&
        config.issuer_url.back() != '/') {
        config.issuer_url += '/';
    }
    return recognised;
}

void OIDCProviderPresets::applyGooglePreset(OIDCConfig& config) {
    CROW_LOG_DEBUG << "Applying Google Workspace OIDC preset";

    // Google uses well-known issuer
    if (config.issuer_url.empty()) {
        config.issuer_url = "https://accounts.google.com";
    }

    // Default claim mappings for Google
    if (!config.username_claim_explicit && (config.username_claim.empty() || config.username_claim == "sub")) {
        config.username_claim = "email";  // Google: use email as username
    }

    // Google supports email_verified
    if (config.email_claim.empty()) {
        config.email_claim = "email";
    }

    // Default scopes for Google (if not already set)
    if (config.scopes.empty()) {
        config.scopes = {"openid", "profile", "email"};
    }

    // Google doesn't use nested role claims
    if (config.roles_claim.empty()) {
        config.roles_claim = "roles";  // Custom claim if added by admin
    }

    CROW_LOG_DEBUG << "Google preset: issuer=" << config.issuer_url
                   << ", username_claim=" << config.username_claim;
}

void OIDCProviderPresets::applyMicrosoftPreset(OIDCConfig& config) {
    CROW_LOG_DEBUG << "Applying Microsoft Azure AD OIDC preset";

    // Microsoft requires tenant in issuer URL
    // Check if issuer has placeholder that needs substitution
    if (config.issuer_url.empty() || config.issuer_url.find("{") != std::string::npos) {
        if (config.issuer_url.empty()) {
            config.issuer_url = "https://login.microsoftonline.com/{tenant}/v2.0";
        }
        CROW_LOG_DEBUG << "Microsoft preset requires tenant substitution in issuer_url: "
                      << config.issuer_url;
    }

    // Microsoft claim mappings
    if (!config.username_claim_explicit && (config.username_claim.empty() || config.username_claim == "sub")) {
        config.username_claim = "preferred_username";  // Azure AD: use preferred_username
    }

    if (config.email_claim.empty()) {
        config.email_claim = "email";
    }

    // Microsoft app roles are in a custom claim
    if (config.roles_claim.empty()) {
        config.roles_claim = "roles";  // Or could use appRoles with nested path
    }

    // Default scopes for Microsoft
    if (config.scopes.empty()) {
        config.scopes = {"openid", "profile", "email"};
    }

    CROW_LOG_DEBUG << "Microsoft preset: issuer_url (needs tenant)=" << config.issuer_url
                   << ", username_claim=" << config.username_claim;
}

void OIDCProviderPresets::applyKeycloakPreset(OIDCConfig& config) {
    CROW_LOG_DEBUG << "Applying Keycloak OIDC preset";

    // Keycloak requires realm in issuer URL
    // Example: https://keycloak.example.com/realms/myrealm
    if (config.issuer_url.empty() || config.issuer_url.find("{") != std::string::npos) {
        if (config.issuer_url.empty()) {
            config.issuer_url = "https://{domain}/realms/{realm}";
        }
        CROW_LOG_DEBUG << "Keycloak preset requires realm substitution in issuer_url: "
                      << config.issuer_url;
    }

    // Keycloak claim mappings
    if (!config.username_claim_explicit && (config.username_claim.empty() || config.username_claim == "sub")) {
        config.username_claim = "preferred_username";  // Keycloak: use preferred_username
    }

    if (config.email_claim.empty()) {
        config.email_claim = "email";
    }

    // Keycloak stores roles in nested claims: realm_access.roles and client_access.{client}.roles
    if (config.role_claim_path.empty()) {
        config.role_claim_path = "realm_access.roles";  // Default to realm roles
    }

    if (config.roles_claim.empty()) {
        config.roles_claim = "roles";  // Fallback simple claim name
    }

    // Keycloak groups claim
    if (config.groups_claim.empty()) {
        config.groups_claim = "groups";
    }

    // Default scopes for Keycloak
    if (config.scopes.empty()) {
        config.scopes = {"openid", "profile", "email"};
    }

    CROW_LOG_DEBUG << "Keycloak preset: issuer_url (needs realm)=" << config.issuer_url
                   << ", role_claim_path=" << config.role_claim_path;
}

void OIDCProviderPresets::applyAuth0Preset(OIDCConfig& config) {
    CROW_LOG_DEBUG << "Applying Auth0 OIDC preset";

    // Auth0 requires domain in issuer URL
    // Example: https://your-domain.auth0.com
    if (config.issuer_url.empty() || config.issuer_url.find("{") != std::string::npos) {
        if (config.issuer_url.empty()) {
            // `domain: acme` is the tenant name; a domain with a dot in it is
            // already the full host (a regional tenant such as
            // acme.eu.auth0.com, or a custom domain).
            config.issuer_url = config.domain.find('.') != std::string::npos ? "https://{domain}/"
                                                                             : "https://{domain}.auth0.com/";
        }
        CROW_LOG_DEBUG << "Auth0 preset requires domain substitution in issuer_url: "
                      << config.issuer_url;
    }

    // Auth0 claim mappings
    if (!config.username_claim_explicit && (config.username_claim.empty() || config.username_claim == "sub")) {
        config.username_claim = "email";  // Auth0: typically use email
    }

    if (config.email_claim.empty()) {
        config.email_claim = "email";
    }

    // Auth0 has no standard roles claim: roles come from a namespaced custom
    // claim an Action adds, such as `https://example.com/roles`. Set it with
    // `roles-claim`; a claim name with dots in it is read as a name first
    // (OIDCAuthHandler::findClaim). The preset used to set a placeholder path
    // here, behind a condition that was never true.

    // Default scopes for Auth0
    if (config.scopes.empty()) {
        config.scopes = {"openid", "profile", "email"};
    }

    CROW_LOG_DEBUG << "Auth0 preset: issuer_url (needs domain)=" << config.issuer_url
                   << ", username_claim=" << config.username_claim;
}

void OIDCProviderPresets::applyOktaPreset(OIDCConfig& config) {
    CROW_LOG_DEBUG << "Applying Okta OIDC preset";

    // Okta requires domain in issuer URL
    // Can also include authorization server: https://{domain}.okta.com/oauth2/{auth_server_id}
    if (config.issuer_url.empty() || config.issuer_url.find("{") != std::string::npos) {
        if (config.issuer_url.empty()) {
            config.issuer_url = "https://{domain}.okta.com/oauth2/default";
        }
        CROW_LOG_DEBUG << "Okta preset requires domain substitution in issuer_url: "
                      << config.issuer_url;
    }

    // Okta claim mappings
    if (!config.username_claim_explicit && (config.username_claim.empty() || config.username_claim == "sub")) {
        config.username_claim = "preferred_username";  // Okta: use preferred_username
    }

    if (config.email_claim.empty()) {
        config.email_claim = "email";
    }

    // Okta supports app roles
    if (config.roles_claim.empty()) {
        config.roles_claim = "roles";
    }

    // Okta groups claim
    if (config.groups_claim.empty()) {
        config.groups_claim = "groups";
    }

    // Default scopes for Okta
    if (config.scopes.empty()) {
        config.scopes = {"openid", "profile", "email"};
    }

    CROW_LOG_DEBUG << "Okta preset: issuer_url (needs domain)=" << config.issuer_url
                   << ", username_claim=" << config.username_claim;
}

std::string OIDCProviderPresets::getRequiredParameters(const std::string& raw_provider_type) {
    const std::string provider_type = normalizeProviderType(raw_provider_type);
    if (provider_type == "google") {
        return "Requires: client-id, allowed-audiences\nOptional: client-secret (for client credentials)";
    } else if (provider_type == "microsoft") {
        return "Requires: client-id, and tenant-id (or a literal issuer-url)\nOptional: client-secret";
    } else if (provider_type == "keycloak") {
        return "Requires: client-id, and domain + realm (or a literal issuer-url)\nOptional: client-secret, role-claim-path";
    } else if (provider_type == "auth0") {
        return "Requires: client-id, and domain (or a literal issuer-url)\nOptional: client-secret, role-claim-path";
    } else if (provider_type == "okta") {
        return "Requires: client-id, and domain (or a literal issuer-url)\nOptional: client-secret";
    } else if (provider_type == "github") {
        return kGitHubUnsupported;
    } else {
        return "Generic OIDC: Requires issuer-url (discovery endpoint), client-id\nOptional: client-secret";
    }
}

std::string OIDCProviderPresets::validateProviderConfig(const OIDCConfig& config) {
    // Runs on a config the preset has been applied to, so placeholders that
    // had a value are already substituted. What is checked is the RESULT:
    // a real issuer URL is valid however it was written. The previous check
    // demanded that the {tenant}/{realm}/{domain} placeholder still be
    // present - which nothing ever substituted - so a correct, literal
    // issuer URL logged "OIDC configuration error" on every handler.
    const std::string provider = normalizeProviderType(config.provider_type);

    if (provider == "github") {
        return kGitHubUnsupported;
    }

    if (config.issuer_url.empty()) {
        return (provider.empty() || provider == "generic")
                   ? "Generic OIDC requires 'issuer-url' to be specified"
                   : provider + " OIDC requires 'issuer-url' to be specified";
    }

    for (const auto& placeholder : extractPlaceholders(config.issuer_url)) {
        std::string key = placeholder;
        if (placeholder == "tenant" || placeholder == "tenant-id") {
            key = "tenant-id";
        }
        return "'issuer-url' (" + config.issuer_url + ") still contains the {" + placeholder +
               "} placeholder. Set `" + key + "` in the oidc block, or write the provider's "
               "real issuer URL into `issuer-url`.";
    }

    if (provider.empty() || provider == "generic") {
        return "";  // Valid
    }

    // All providers require client_id
    if (config.client_id.empty()) {
        return provider + " OIDC requires 'client-id' to be specified";
    }

    return "";  // Valid
}

bool OIDCProviderPresets::hasPlaceholders(const std::string& issuer_url) {
    return issuer_url.find("{") != std::string::npos &&
           issuer_url.find("}") != std::string::npos;
}

std::vector<std::string> OIDCProviderPresets::extractPlaceholders(const std::string& issuer_url) {
    std::vector<std::string> placeholders;
    size_t pos = 0;

    while ((pos = issuer_url.find("{", pos)) != std::string::npos) {
        size_t end_pos = issuer_url.find("}", pos);
        if (end_pos == std::string::npos) {
            break;  // Malformed placeholder
        }

        std::string placeholder = issuer_url.substr(pos + 1, end_pos - pos - 1);
        placeholders.push_back(placeholder);
        pos = end_pos + 1;
    }

    return placeholders;
}

} // namespace flapi
