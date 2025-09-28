#pragma once

#include <string>

namespace flapi {

class PathUtils {
public:
    /**
     * Convert a URL path to a URL-safe slug for use in route parameters
     * Examples:
     *   "/customers/" -> "customers-slash"
     *   "/publicis" -> "publicis"
     *   "/sap/functions" -> "sap-slash-functions"
     *   "" -> "empty"
     */
    static std::string pathToSlug(const std::string& path);
    
    /**
     * Convert a slug back to the original URL path
     * Examples:
     *   "customers-slash" -> "/customers/"
     *   "publicis" -> "/publicis"
     *   "sap-slash-functions" -> "/sap/functions"
     *   "empty" -> ""
     */
    static std::string slugToPath(const std::string& slug);
    
private:
    static const std::string SLASH_REPLACEMENT;
    static const std::string EMPTY_REPLACEMENT;
};

} // namespace flapi
