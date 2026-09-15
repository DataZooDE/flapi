#include "trace_capture_policy.hpp"

#include <algorithm>
#include <array>
#include <cctype>

namespace flapi {

namespace {

std::string toLower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// Never legitimate span content at ANY tier, and an operator must not have to
// remember to list them. The plan's invariant - Authorization headers, bearer
// tokens, passwords, connection strings and credential-manager material never
// appear - cannot be allowed to depend on configuration being complete.
constexpr std::array<std::string_view, 14> kAlwaysRedacted{{
    "authorization", "proxy-authorization", "cookie", "set-cookie",
    "password", "passwd", "secret", "client_secret",
    "api_key", "apikey", "access_token", "refresh_token",
    "private_key", "connection_string",
}};

// Truncate on a UTF-8 character boundary, never mid-sequence.
std::size_t utf8SafeLength(std::string_view s, std::size_t max_bytes) {
    if (s.size() <= max_bytes) {
        return s.size();
    }
    std::size_t cut = max_bytes;
    // Walk back over continuation bytes (10xxxxxx) to the start of the sequence
    // they belong to.
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    return cut;
}

}  // namespace

CapturePolicy::CapturePolicy(CaptureTier global,
                             std::unordered_set<std::string> redact_keys,
                             std::size_t max_value_bytes,
                             std::size_t max_documents)
    : global_(global), max_value_bytes_(max_value_bytes), max_documents_(max_documents) {
    // Lowercase once, so matching is case-insensitive without per-call work.
    // A parameter named "Token" must not slip past a list containing "token":
    // HTTP parameter names are not reliably normalised and the failure mode is a
    // leaked credential.
    for (const auto& key : redact_keys) {
        redact_keys_.insert(toLower(key));
    }
}

CaptureTier CapturePolicy::effectiveTier(const std::optional<CaptureTier>& endpoint) const {
    if (global_ == CaptureTier::Off) {
        return CaptureTier::Off;   // one lever that always stops export
    }
    return endpoint.value_or(global_);
}

bool CapturePolicy::isAlwaysRedacted(std::string_view key) const {
    const auto lowered = toLower(key);
    return std::find(kAlwaysRedacted.begin(), kAlwaysRedacted.end(), lowered)
           != kAlwaysRedacted.end();
}

std::string CapturePolicy::redactAndClamp(std::string_view key, std::string_view value) const {
    if (isAlwaysRedacted(key) || redact_keys_.count(toLower(key)) > 0) {
        return "<redacted>";
    }
    return std::string(value.substr(0, utf8SafeLength(value, max_value_bytes_)));
}

bool CapturePolicy::allowQueryText(int interpolated_count, bool opt_in) const {
    return opt_in && interpolated_count == 0;
}

}  // namespace flapi
