#pragma once

#include <string>
#include <vector>
#include <regex>
#include <map>

namespace flapi {

class RouteTranslator {
public:
    /// "" when `url_path` is a route flapi can match, otherwise why not. A
    /// route is `/` or `/`-separated segments, each a literal of
    /// [A-Za-z0-9._~-] or a `:name` parameter ([A-Za-z_][A-Za-z0-9_]*), with
    /// at most one trailing `/`.
    static std::string urlPathProblem(const std::string& url_path);
    /// Whether some request path matches both routes.
    static bool routesOverlap(const std::string& a, const std::string& b);
    /// Whether every request path `specific` matches is also matched by
    /// `general` (`/a/:x/:y` covers `/a/b/c`, `/a/b/:y` and `/a/:x/c`; `/a/b/:y`
    /// does not cover `/a/:x/c`).
    static bool routeCovers(const std::string& general, const std::string& specific);
    /// Which route wins a request both match: segment by segment from the
    /// left, the first literal-vs-parameter difference decides and the literal
    /// wins. < 0: `a` is more specific, > 0: `b`, 0: the same shape.
    static int compareSpecificity(const std::string& a, const std::string& b);

    static std::string translateRoutePath(const std::string& flapiPath, std::vector<std::string>& paramNames);
    /// Decode one captured path value (RFC 3986 percent-encoding, nothing
    /// else: '+' stays a plus, unlike a query string). The route is matched on
    /// the RAW path, so an encoded "%2F" has already stayed inside its one
    /// segment; this runs afterwards and decodes exactly once. Returns false
    /// for a malformed escape ("%zz", a truncated "%4"), an encoded NUL, or a
    /// result that is not valid UTF-8; `out` is then unspecified.
    static bool percentDecodePathValue(const std::string& raw, std::string& out);

    static bool matchAndExtractParams(const std::string& routePattern, const std::string& actualPath, 
                                      std::vector<std::string>& paramNames, std::map<std::string, std::string>& pathParams);

private:
    static const std::regex param_regex;
};

} // namespace flapi