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
     *   "/order-items"    -> "-order~1items"
     *   "/"               -> "-"
     *   ""                -> "empty"
     *
     * '/' becomes '-', so the common case stays readable; a literal '-' or
     * '~' is escaped JSON-pointer style ("~1", "~0"), which makes a '-' in a
     * slug unambiguously a slash. The alphabet contains no '%' precisely so
     * that neither encodeURIComponent nor RFC 3986 normalisation can alter a
     * slug in transit. Verified exhaustively over every path up to length 5
     * drawn from "/-~01ab": 19,608 paths, no collisions, no round-trip
     * failures.
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

    /// True when `identifier` is a url-path rather than a slug.
    ///
    /// A slug never contains '/', because the encoder maps every '/' to '-'.
    static bool looksLikePath(const std::string& identifier);

    /// Resolve either form the config service accepts - a slug, or a
    /// percent-decoded url-path - to a url-path.
    ///
    /// Two explicit forms rather than one tolerant decoder: making slugToPath
    /// prepend a missing leading slash made 'x' and '-x' both address '/x',
    /// so every endpoint had two config-service names again.
    static std::string identifierToPath(const std::string& identifier);
    
private:
    static const std::string EMPTY_REPLACEMENT;
};

} // namespace flapi
