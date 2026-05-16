#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace flapi {

// Per-tool response-shaping policy (W2.4). Applied to a tool's JSON-array
// result *after* execution and *before* it leaves the server, so the agent
// only sees what the operator wants it to see.
//
// All fields are optional and inert by default — the empty config is a
// no-op, preserving the simple-first-experience promise.
struct ResponseShape {
    // Cap the array length to this many rows. 0 means "all rows are
    // suppressed"; std::nullopt means "no cap".
    std::optional<std::size_t> max_rows;

    // Replace these column values with the literal string "<redacted>"
    // in every row. Missing columns are tolerated (no-op).
    std::vector<std::string> redact_columns;

    // When true, suppress row data entirely and emit a summary object:
    // { row_count, columns: [...], sampled: true }.
    bool sample = false;

    bool isNoOp() const {
        return !max_rows.has_value() && redact_columns.empty() && !sample;
    }
};

// Pure transformer: takes a JSON payload (typically a top-level array
// emitted by MCPToolHandler::formatResult) and applies the shape config.
// No dependency on QueryResult, ConfigManager, or any handler internals —
// the only contract is "JSON string in, JSON string out".
class MCPResponseShaper {
public:
    static constexpr const char* kRedactedSentinel = "<redacted>";

    std::string shape(const std::string& json_payload, const ResponseShape& config) const;
};

} // namespace flapi
