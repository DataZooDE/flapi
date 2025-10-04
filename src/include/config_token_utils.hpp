#pragma once

#include <string>

namespace flapi {

/**
 * Utility functions for config service token generation and validation
 */
class ConfigTokenUtils {
public:
    /**
     * Generate a cryptographically secure random token
     * @param length Token length (default: 32)
     * @return Random alphanumeric token
     */
    static std::string generateSecureToken(std::size_t length = 32);
    
    /**
     * Validate token format
     * @param token Token to validate
     * @return true if token is valid format (alphanumeric, correct length)
     */
    static bool isValidTokenFormat(const std::string& token);
};

} // namespace flapi

