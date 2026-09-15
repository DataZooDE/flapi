#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>

#include "tracing_config.hpp"

namespace flapi {

// Decides what a span is allowed to contain.
//
// Default posture (D2): tracing off. Enabled means METADATA only - structure and
// shape, never values. Payload capture requires an explicit per-endpoint opt-in,
// so an operator cannot reach data export by accident.
//
// "Metadata" means: endpoint and template identity, parameter NAMES and declared
// types, row and column counts, byte counts, durations, cache-backing,
// enumerated error kinds. Never an argument value, never a result row, never a
// filled path or query string, never a header, never a credential.
class CapturePolicy {
public:
    CapturePolicy(CaptureTier global,
                  std::unordered_set<std::string> redact_keys,
                  std::size_t max_value_bytes,
                  std::size_t max_documents);

    // A per-endpoint setting may move the tier in EITHER direction, but a global
    // `off` always wins. That gives an operator one lever guaranteed to stop
    // export, which is what a security review asks for.
    CaptureTier effectiveTier(const std::optional<CaptureTier>& endpoint) const;

    bool capturesValues(const std::optional<CaptureTier>& endpoint) const {
        return effectiveTier(endpoint) == CaptureTier::Payload;
    }

    // Redact FIRST, clamp SECOND. Clamping first can truncate mid-value and leave
    // a partial secret in the span, and a partial secret is still a secret.
    // Clamping is UTF-8 aware: a truncated multi-byte sequence is invalid UTF-8
    // and a backend may reject the whole span rather than one attribute.
    std::string redactAndClamp(std::string_view key, std::string_view value) const;

    // db.query.text is unusual: because flAPI binds typed parameters as DuckDB
    // prepared statements, the parameterised SQL holds placeholders rather than
    // values. That is safe - but only when EVERY site is a prepared binding. An
    // interpolated site substitutes a value into the SQL text itself.
    bool allowQueryText(int interpolated_count, bool opt_in) const;

    std::size_t maxValueBytes() const { return max_value_bytes_; }
    std::size_t maxDocuments() const { return max_documents_; }

private:
    bool isAlwaysRedacted(std::string_view key) const;

    CaptureTier global_;
    std::unordered_set<std::string> redact_keys_;   // lowercased on construction
    std::size_t max_value_bytes_;
    std::size_t max_documents_;
};

}  // namespace flapi
