#pragma once

#include <optional>
#include <string>
#include <vector>

namespace flapi {

// W1.2: CORS allowlist policy. Pure function over a configured allowlist
// and the request's Origin header. Returns the value that should land in
// the `Access-Control-Allow-Origin` response header, or std::nullopt
// when the request must not be granted any CORS access (browser blocks).
//
// Backward-compatibility rule: an empty allowlist preserves the
// historic "*" default so `flapii project init` demos keep working.
class CorsPolicy {
public:
    static constexpr const char* kWildcard = "*";

    std::optional<std::string> resolveAllowedOrigin(
        const std::string& request_origin,
        const std::vector<std::string>& allow_origins) const;

    // How long a browser may cache a preflight answer, in seconds.
    static constexpr int kPreflightMaxAgeSeconds = 600;

    // Headers a browser must be told by name: the Fetch spec does not let the
    // `*` wildcard cover `Authorization`, so a server that authenticates must
    // list it (and the other headers flAPI's clients send) explicitly.
    static const std::vector<std::string>& credentialHeaders();

    // `Access-Control-Allow-Headers` for a preflight. A configured
    // `cors.allow-headers` list is used as written. Without one (or with
    // `*` in it), the headers the browser asked for in
    // `Access-Control-Request-Headers` are echoed; when it asked for none, the
    // answer is `*`, plus the credentialHeaders() by name when authentication
    // is enabled.
    std::string resolveAllowHeaders(const std::vector<std::string>& configured,
                                    const std::string& requested_headers,
                                    bool auth_enabled) const;

    // `Access-Control-Allow-Methods` for a preflight: the configured
    // `cors.allow-methods`, or GET, POST, PUT, PATCH, DELETE.
    std::string resolveAllowMethods(const std::vector<std::string>& configured) const;
};

} // namespace flapi
