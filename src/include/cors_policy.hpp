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
};

} // namespace flapi
