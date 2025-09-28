#include "path_utils.hpp"
#include <algorithm>
#include <regex>

namespace flapi {

const std::string PathUtils::SLASH_REPLACEMENT = "-slash-";
const std::string PathUtils::EMPTY_REPLACEMENT = "empty";

std::string PathUtils::pathToSlug(const std::string& path) {
    if (path.empty()) {
        return EMPTY_REPLACEMENT;
    }
    
    std::string slug = path;
    
    // Remove leading slash if present
    if (slug.front() == '/') {
        slug = slug.substr(1);
    }
    
    // Handle trailing slash
    bool hasTrailingSlash = false;
    if (!slug.empty() && slug.back() == '/') {
        hasTrailingSlash = true;
        slug = slug.substr(0, slug.length() - 1);
    }
    
    // Replace internal slashes with our replacement
    std::replace(slug.begin(), slug.end(), '/', '-');
    
    // Replace any remaining special characters with hyphens
    slug = std::regex_replace(slug, std::regex("[^a-zA-Z0-9\\-_]"), "-");
    
    // Remove multiple consecutive hyphens
    slug = std::regex_replace(slug, std::regex("-+"), "-");
    
    // Remove leading/trailing hyphens
    if (!slug.empty() && slug.front() == '-') {
        slug = slug.substr(1);
    }
    if (!slug.empty() && slug.back() == '-') {
        slug = slug.substr(0, slug.length() - 1);
    }
    
    // Add trailing slash indicator
    if (hasTrailingSlash) {
        slug += "-slash";
    }
    
    return slug.empty() ? EMPTY_REPLACEMENT : slug;
}

std::string PathUtils::slugToPath(const std::string& slug) {
    if (slug == EMPTY_REPLACEMENT) {
        return "";
    }
    
    std::string path = slug;
    
    // Check for trailing slash indicator
    bool hasTrailingSlash = false;
    if (path.length() >= 6 && path.substr(path.length() - 6) == "-slash") {
        hasTrailingSlash = true;
        path = path.substr(0, path.length() - 6);
    }
    
    // Replace hyphens back to slashes (but be careful about our special replacements)
    std::replace(path.begin(), path.end(), '-', '/');
    
    // Add leading slash
    if (!path.empty()) {
        path = "/" + path;
    }
    
    // Add trailing slash if needed
    if (hasTrailingSlash) {
        path += "/";
    }
    
    return path;
}

} // namespace flapi
