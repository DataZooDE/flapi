#pragma once

#include <string>

namespace flapi {

// W1.4: How the rate-limit middleware groups requests into buckets.
// `Ip` is the historic default (per-client-ip + per-path).
// `User` keys on the caller's `Authorization` header (hashed); all
// requests carrying the same Authorization land in the same bucket
// regardless of IP. Anonymous requests share one anonymous bucket.
// `UserOrIp` uses the user identifier when an Authorization header is
// present and falls back to the IP otherwise — useful when a deployment
// mixes authenticated and anonymous traffic.
enum class RateLimitKeyStrategy {
    Ip,
    User,
    UserOrIp,
};

class RateLimitKeyStrategyUtils {
public:
    // Parse a config string into the enum. Unknown / empty values fall
    // back to `Ip` so existing configs keep working unchanged.
    static RateLimitKeyStrategy parse(const std::string& value);
    static std::string toString(RateLimitKeyStrategy strategy);
};

// Pure helper: build the bucket key used by RateLimitMiddleware.
// The output is deterministic, never contains the raw Authorization
// value (it is hashed), and always includes the path so that two
// endpoints stay in separate buckets even for the same caller.
class RateLimitKeyBuilder {
public:
    std::string buildKey(RateLimitKeyStrategy strategy,
                         const std::string& client_ip,
                         const std::string& authorization_header,
                         const std::string& path) const;
};

} // namespace flapi
