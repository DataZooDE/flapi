#pragma once

#include <string>
#include <vector>
#include <optional>
#include "config_manager.hpp"

namespace flapi {

/**
 * OIDC Provider Presets
 * Auto-configures OIDC settings for well-known providers
 */
class OIDCProviderPresets {
public:
    /**
     * Apply provider-specific presets to OIDCConfig
     * Fills in missing fields based on provider type
     * @param config Configuration to modify (in-place)
     * @return true if provider recognized, false if generic/unknown
     */
    static bool applyPreset(OIDCConfig& config);

    /**
     * Check if provider type requires additional parameters
     * @param provider_type Provider type string
     * @return Description of required parameters
     */
    static std::string getRequiredParameters(const std::string& provider_type);

    /**
     * Validate provider-specific configuration
     * @param config Configuration to validate
     * @return Error message if invalid, empty string if valid
     */
    static std::string validateProviderConfig(const OIDCConfig& config);

private:
    /**
     * Apply Google Workspace preset
     */
    static void applyGooglePreset(OIDCConfig& config);

    /**
     * Apply Microsoft Azure AD preset
     */
    static void applyMicrosoftPreset(OIDCConfig& config);

    /**
     * Apply Keycloak preset
     */
    static void applyKeycloakPreset(OIDCConfig& config);

    /**
     * Apply Auth0 preset
     */
    static void applyAuth0Preset(OIDCConfig& config);

    /**
     * Apply Okta preset
     */
    static void applyOktaPreset(OIDCConfig& config);

    /**
     * Apply GitHub OAuth preset
     */
    static void applyGitHubPreset(OIDCConfig& config);

    /**
     * Check if issuer URL contains placeholder that needs substitution
     * @param issuer_url URL with potential placeholders like {domain}, {tenant}, {realm}
     * @return true if placeholders found
     */
    static bool hasPlaceholders(const std::string& issuer_url);

    /**
     * Extract placeholder value from issuer URL
     * E.g., "https://{domain}.auth0.com" → "domain"
     * @param issuer_url URL with placeholder
     * @return vector of placeholder names
     */
    static std::vector<std::string> extractPlaceholders(const std::string& issuer_url);
};

} // namespace flapi
