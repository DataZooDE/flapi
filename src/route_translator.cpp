#include "route_translator.hpp"

#include <mutex>
#include <regex>
#include <shared_mutex>
#include <unordered_map>

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
    compiled->regex = std::regex(regex_pattern);

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
        crowPath += flapiPath[i];
        ++i;
    }

    return "^" + crowPath + "$";
}

bool RouteTranslator::matchAndExtractParams(const std::string& routePattern, const std::string& actualPath,
                                            std::vector<std::string>& paramNames, std::map<std::string, std::string>& pathParams) {
    const auto compiled = compiledRouteFor(routePattern);

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

} // namespace flapi
