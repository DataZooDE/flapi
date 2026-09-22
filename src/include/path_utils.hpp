#pragma once

#include <string>

namespace flapi {

class PathUtils {
public:
    /**
     * Convert a URL path to a URL-safe slug, injectively.
     *
     *   "/customers/"     -> "-customers-"
     *   "/sap/functions"  -> "-sap-functions"
     *   "/order-items"    -> "-order%2Ditems"
     *   "/"               -> "-"
     *   ""                -> "empty"
     *
     * '/' becomes '-', so the common case stays readable; a literal '-' or '%'
     * is percent-escaped, which is what makes a '-' in a slug unambiguously a
     * slash. Verified exhaustively over every path up to length 5 drawn from
     * "/-%2Dab": 19,608 paths, no collisions, no round-trip failures.
     *
     * The previous codec was NOT injective - "/a-b" and "/a/b" both became
     * "a-b" and both decoded to "/a/b", so an endpoint whose URL contained a
     * hyphen was addressed as, and rewritten to, a different endpoint (#123).
     */
    static std::string pathToSlug(const std::string& path);

    /**
     * Inverse of pathToSlug. slugToPath(pathToSlug(p)) == p for every p.
     */
    static std::string slugToPath(const std::string& slug);
    
private:
    static const std::string EMPTY_REPLACEMENT;
};

} // namespace flapi
