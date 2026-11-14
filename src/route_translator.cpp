#include "route_translator.hpp"

#include <crow/logging.h>

#include <mutex>
#include <regex>
#include <shared_mutex>
#include <string_view>
#include <unordered_map>
#include <algorithm>
#include <cctype>

namespace flapi {

const std::regex RouteTranslator::param_regex(":([^/]+)");

namespace {

// A route pattern compiles to the same regex every time - it is derived from
// EndpointConfig::urlPath, which only changes on config reload. Rebuilding it per
// call cost ~6us for a one-parameter route, and ConfigManager::getEndpointForPathAndMethod
// scans every endpoint calling matchesPath, from three separate places per request
// (RateLimitMiddleware, AuthMiddleware, handleDynamicRequest). On the examples
// config that was ~160us of pure routing per request, against a ~200us
// time-to-first-byte for /health - plausibly the bulk of the unexplained
// "degrades under high concurrent load" behaviour.
//
// So: compile once, keyed by pattern. Patterns come from configuration, never
// from the request, so the key space is bounded by the endpoint count; the cap
// below is belt-and-braces against a future caller passing request-derived text.
struct CompiledRoute {
    std::regex regex;
    std::vector<std::string> param_names;
    // False when the pattern did not compile: such a route matches nothing.
    bool valid = true;
};

constexpr std::size_t kMaxCachedRoutes = 4096;

std::shared_mutex& cacheMutex() {
    static std::shared_mutex m;
    return m;
}

std::unordered_map<std::string, std::shared_ptr<const CompiledRoute>>& cache() {
    static std::unordered_map<std::string, std::shared_ptr<const CompiledRoute>> c;
    return c;
}

std::shared_ptr<const CompiledRoute> compiledRouteFor(const std::string& pattern) {
    {
        std::shared_lock<std::shared_mutex> read(cacheMutex());
        auto it = cache().find(pattern);
        if (it != cache().end()) {
            return it->second;
        }
    }

    // Build outside the write lock; a concurrent duplicate build is cheaper than
    // serialising every miss, and the result is identical either way.
    auto compiled = std::make_shared<CompiledRoute>();
    const std::string regex_pattern =
        RouteTranslator::translateRoutePath(pattern, compiled->param_names);
    // A pattern that does not compile used to throw from here, inside
    // routing, on EVERY request that reached it in the endpoint scan - the
    // worker died without answering, so one bad url-path (`/customers/{cid}`)
    // hung the whole server, /health included. url-paths are validated at
    // load and literals are escaped below, so this is belt-and-braces: the
    // route matches nothing and says why, once.
    try {
        compiled->regex = std::regex(regex_pattern);
    } catch (const std::regex_error& e) {
        CROW_LOG_ERROR << "Route " << pattern << " cannot be matched (" << e.what()
                       << "); it answers no request. Fix its url-path.";
        compiled->valid = false;
    }

    std::unique_lock<std::shared_mutex> write(cacheMutex());
    if (cache().size() >= kMaxCachedRoutes) {
        // Configuration cannot realistically reach this; if it does, stop growing
        // rather than serve a memory-exhaustion vector.
        return compiled;
    }
    auto [it, inserted] = cache().emplace(pattern, std::move(compiled));
    return it->second;
}

}  // namespace

std::string RouteTranslator::translateRoutePath(const std::string& flapiPath, std::vector<std::string>& paramNames) {
    std::string crowPath;
    crowPath.reserve(flapiPath.size() + 16);

    // Single pass, replacing ":name" with a capture group as it goes. The previous
    // implementation ran a std::regex_replace - constructing a fresh std::regex -
    // once per parameter, on top of the sregex_iterator scan.
    std::size_t i = 0;
    while (i < flapiPath.size()) {
        if (flapiPath[i] == ':') {
            std::size_t start = i + 1;
            std::size_t end = start;
            while (end < flapiPath.size() && flapiPath[end] != '/') {
                ++end;
            }
            if (end > start) {
                paramNames.push_back(flapiPath.substr(start, end - start));
                crowPath += "([^/]+)";
                i = end;
                continue;
            }
        }
        // A literal is matched literally. Copied raw, `.` matched any
        // character and `{`, `(`, `[` made the pattern fail to compile.
        if (std::string_view("\\^$.|?*+()[]{}").find(flapiPath[i]) != std::string_view::npos) {
            crowPath += '\\';
        }
        crowPath += flapiPath[i];
        ++i;
    }

    return "^" + crowPath + "$";
}

bool RouteTranslator::matchAndExtractParams(const std::string& routePattern, const std::string& actualPath,
                                            std::vector<std::string>& paramNames, std::map<std::string, std::string>& pathParams) {
    const auto compiled = compiledRouteFor(routePattern);

    if (!compiled->valid) {
        return false;
    }
    std::smatch matches;
    if (std::regex_match(actualPath, matches, compiled->regex)) {
        for (size_t i = 1; i < matches.size(); ++i) {
            pathParams[compiled->param_names[i - 1]] = matches[i].str();
        }
        paramNames = compiled->param_names;
        return true;
    }

    return false;
}

namespace {

struct RouteSegment {
    bool param;
    std::string text;
};

// The segments a url-path is matched by, with translateRoutePath's notion of a
// parameter (`:` followed by at least one character). A trailing `/` is an
// empty literal segment, which no parameter matches (`[^/]+`).
std::vector<RouteSegment> routeSegments(const std::string& url_path) {
    std::vector<RouteSegment> segments;
    std::size_t start = (!url_path.empty() && url_path.front() == '/') ? 1 : 0;
    while (true) {
        const std::size_t end = url_path.find('/', start);
        const std::string part = url_path.substr(start, end == std::string::npos ? std::string::npos : end - start);
        segments.push_back({part.size() > 1 && part.front() == ':', part});
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return segments;
}

bool isLiteralChar(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
           c == '~' || c == '-';
}

}  // namespace

std::string RouteTranslator::urlPathProblem(const std::string& url_path) {
    if (url_path.empty() || url_path.front() != '/') {
        return "url-path '" + url_path + "' must start with /";
    }
    if (url_path == "/") {
        return "";
    }
    const auto segments = routeSegments(url_path);
    for (std::size_t i = 0; i < segments.size(); ++i) {
        const auto& segment = segments[i];
        const std::string& text = segment.text;
        if (text.empty()) {
            if (i + 1 == segments.size()) {
                continue;   // one trailing slash: /customers/
            }
            return "url-path '" + url_path + "' has an empty segment (`//`)";
        }
        if (text.front() == ':') {
            const std::string name = text.substr(1);
            bool ok = !name.empty() && (std::isalpha(static_cast<unsigned char>(name.front())) || name.front() == '_');
            for (const char c : name) {
                ok = ok && (std::isalnum(static_cast<unsigned char>(c)) || c == '_');
            }
            if (!ok) {
                return "url-path '" + url_path + "': parameter segment '" + text +
                       "' must be `:name` with a name of letters, digits and _ (not starting with a digit)";
            }
            continue;
        }
        for (const char c : text) {
            if (!isLiteralChar(c)) {
                std::string hint;
                if (text.size() > 2 && text.front() == '{' && text.back() == '}') {
                    hint = "; path parameters are written `:" + text.substr(1, text.size() - 2) + "`, not `" + text +
                           "`";
                }
                return "url-path '" + url_path + "': segment '" + text + "' contains '" + std::string(1, c) +
                       "'; a literal segment may only use letters, digits and . _ ~ -" + hint;
            }
        }
    }
    return "";
}

bool RouteTranslator::routesOverlap(const std::string& a, const std::string& b) {
    const auto sa = routeSegments(a);
    const auto sb = routeSegments(b);
    if (sa.size() != sb.size()) {
        return false;
    }
    for (std::size_t i = 0; i < sa.size(); ++i) {
        if (!sa[i].param && !sb[i].param) {
            if (sa[i].text != sb[i].text) {
                return false;
            }
        } else if (!sa[i].param || !sb[i].param) {
            // A parameter matches one or more characters, never the empty
            // segment after a trailing slash.
            if ((sa[i].param ? sb[i] : sa[i]).text.empty()) {
                return false;
            }
        }
    }
    return true;
}

int RouteTranslator::compareSpecificity(const std::string& a, const std::string& b) {
    const auto sa = routeSegments(a);
    const auto sb = routeSegments(b);
    const std::size_t n = std::min(sa.size(), sb.size());
    for (std::size_t i = 0; i < n; ++i) {
        if (sa[i].param != sb[i].param) {
            return sa[i].param ? 1 : -1;
        }
    }
    // Routes of different lengths never match one request; ordered anyway so
    // the comparison is total.
    if (sa.size() != sb.size()) {
        return sa.size() < sb.size() ? -1 : 1;
    }
    return 0;
}

} // namespace flapi
