#include "cors_policy.hpp"

#include <algorithm>

namespace flapi {

namespace {

bool containsWildcard(const std::vector<std::string>& allow_origins) {
    return std::any_of(allow_origins.begin(), allow_origins.end(),
                       [](const std::string& v) { return v == CorsPolicy::kWildcard; });
}

bool containsExact(const std::vector<std::string>& allow_origins,
                   const std::string& origin) {
    return std::find(allow_origins.begin(), allow_origins.end(), origin) != allow_origins.end();
}

} // namespace

std::optional<std::string> CorsPolicy::resolveAllowedOrigin(
    const std::string& request_origin,
    const std::vector<std::string>& allow_origins) const {

    // Empty allowlist: keep the historic "*" default. This matters for the
    // "simple stays simple" promise — fresh `flapii project init` projects
    // must still work from a browser without any CORS configuration.
    if (allow_origins.empty()) {
        return std::string(kWildcard);
    }

    // Explicit wildcard always wins, even when combined with concrete
    // entries. Mixing them is a configuration smell but the result is
    // unambiguous.
    if (containsWildcard(allow_origins)) {
        return std::string(kWildcard);
    }

    // Same-origin / curl-style requests don't carry an Origin header.
    // Returning nullopt is correct — no CORS response header is needed
    // for same-origin requests; the browser doesn't enforce CORS on them.
    if (request_origin.empty()) {
        return std::nullopt;
    }

    if (containsExact(allow_origins, request_origin)) {
        return request_origin;
    }

    return std::nullopt;
}

} // namespace flapi
