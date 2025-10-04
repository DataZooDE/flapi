#include "config_token_utils.hpp"

#include <random>
#include <algorithm>
#include <cctype>

namespace flapi {

std::string ConfigTokenUtils::generateSecureToken(std::size_t length) {
    static const char alphanum[] =
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";
    
    static const std::size_t alphanum_size = sizeof(alphanum) - 1;
    
    // Use random_device for cryptographically secure random numbers
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<std::size_t> dis(0, alphanum_size - 1);
    
    std::string token;
    token.reserve(length);
    
    for (std::size_t i = 0; i < length; ++i) {
        token += alphanum[dis(gen)];
    }
    
    return token;
}

bool ConfigTokenUtils::isValidTokenFormat(const std::string& token) {
    if (token.empty()) {
        return false;
    }
    
    // Check if all characters are alphanumeric
    return std::all_of(token.begin(), token.end(), [](unsigned char c) {
        return std::isalnum(c);
    });
}

} // namespace flapi

