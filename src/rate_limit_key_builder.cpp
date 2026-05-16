#include "rate_limit_key_builder.hpp"

#include <functional>
#include <sstream>

namespace flapi {

namespace {

constexpr const char* kAnonymousMarker = "anonymous";

std::string hashAuthHeader(const std::string& authorization_header) {
    // Stable, non-cryptographic hash of the Authorization header. We do
    // not need cryptographic strength — we only need:
    //   1) determinism so the same caller hashes to the same bucket
    //   2) no plaintext token in the key (it gets logged at DEBUG)
    // std::hash satisfies both for our purposes. Operators who want
    // stronger guarantees should put a reverse proxy in front and key
    // on a header it injects.
    const std::size_t h = std::hash<std::string>{}(authorization_header);
    std::ostringstream oss;
    oss << "u" << std::hex << h;
    return oss.str();
}

std::string principalForUserStrategy(const std::string& authorization_header) {
    if (authorization_header.empty()) {
        return std::string(kAnonymousMarker);
    }
    return hashAuthHeader(authorization_header);
}

} // namespace

RateLimitKeyStrategy RateLimitKeyStrategyUtils::parse(const std::string& value) {
    if (value == "user") {
        return RateLimitKeyStrategy::User;
    }
    if (value == "user-or-ip") {
        return RateLimitKeyStrategy::UserOrIp;
    }
    // `ip`, empty, or unknown — preserve historical behaviour.
    return RateLimitKeyStrategy::Ip;
}

std::string RateLimitKeyStrategyUtils::toString(RateLimitKeyStrategy strategy) {
    switch (strategy) {
        case RateLimitKeyStrategy::Ip:        return "ip";
        case RateLimitKeyStrategy::User:      return "user";
        case RateLimitKeyStrategy::UserOrIp:  return "user-or-ip";
    }
    return "ip";
}

std::string RateLimitKeyBuilder::buildKey(RateLimitKeyStrategy strategy,
                                          const std::string& client_ip,
                                          const std::string& authorization_header,
                                          const std::string& path) const {
    std::string principal;
    switch (strategy) {
        case RateLimitKeyStrategy::Ip:
            principal = client_ip;
            break;
        case RateLimitKeyStrategy::User:
            principal = principalForUserStrategy(authorization_header);
            break;
        case RateLimitKeyStrategy::UserOrIp:
            principal = authorization_header.empty()
                            ? client_ip
                            : principalForUserStrategy(authorization_header);
            break;
    }
    return principal + "|" + path;
}

} // namespace flapi
