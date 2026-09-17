#include "trace_capture_policy.hpp"

#include "redaction.hpp"

#include <algorithm>
#include <array>
#include <cctype>

namespace flapi {

namespace {

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
                             std::size_t max_value_bytes)
    : global_(global), max_value_bytes_(max_value_bytes) {
    // Lowercase once, so matching is case-insensitive without per-call work.
    // A parameter named "Token" must not slip past a list containing "token":
    // HTTP parameter names are not reliably normalised and the failure mode is a
    // leaked credential.
    for (const auto& key : redact_keys) {
        redact_keys_.insert(normaliseKey(key));
    }
}

CaptureTier CapturePolicy::effectiveTier(const std::optional<CaptureTier>& endpoint) const {
    if (global_ == CaptureTier::Off) {
        return CaptureTier::Off;   // one lever that always stops export
    }
    return endpoint.value_or(global_);
}

// Shared with the audit log via redaction.hpp, deliberately: a credential list
// that exists in two places diverges, and the half that is forgotten is the one
// that leaks. See src/redaction.cpp for the stems and why they are matched as
// substrings rather than by equality.
bool CapturePolicy::isAlwaysRedacted(std::string_view key) const {
    return isCredentialKey(key);
}

std::string CapturePolicy::redactAndClamp(std::string_view key, std::string_view value) const {
    if (isAlwaysRedacted(key) || redact_keys_.count(normaliseKey(key)) > 0) {
        return "<redacted>";
    }
    return std::string(value.substr(0, utf8SafeLength(value, max_value_bytes_)));
}

bool CapturePolicy::allowQueryText(int interpolated_count, bool opt_in) const {
    return opt_in && interpolated_count == 0;
}

}  // namespace flapi
