#pragma once

#include <string>

namespace flapi {

class PathUtils {
public:
    /**
     * Convert a URL path to a URL-safe slug for use in route parameters.
     *
     * Measured behaviour (the previous comment claimed
     * "/sap/functions" -> "sap-slash-functions", which the code has never
     * done - internal slashes become a plain hyphen; only a TRAILING slash
     * gets the "-slash" marker):
     *   "/customers/"    -> "customers-slash"
     *   "/publicis"      -> "publicis"
     *   "/sap/functions" -> "sap-functions"
     *   ""               -> "empty"
     *
     * NOT injective: a path containing a literal hyphen collides with the
     * same path containing a slash. "/a-b" and "/a/b" both slug to "a-b",
     * and slugToPath turns both back into "/a/b" - so an endpoint whose URL
     * contains a hyphen is addressed, and round-tripped, as a different
     * endpoint. Tracked in #123; fixing it changes the
     * /api/v1/_config/endpoints/{slug} surface, so it is not a refactor.
     */
    static std::string pathToSlug(const std::string& path);
    
    /**
     * Convert a slug back to the original URL path.
     *   "customers-slash" -> "/customers/"
     *   "publicis"        -> "/publicis"
     *   "sap-functions"   -> "/sap/functions"
     *   "empty"           -> ""
     *
     * Every hyphen becomes a slash, so this cannot recover a path that
     * contained a literal hyphen. See pathToSlug and #123.
     */
    static std::string slugToPath(const std::string& slug);
    
private:
    static const std::string EMPTY_REPLACEMENT;
};

} // namespace flapi
