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

class QueryResult;

// Pure transformer: takes a query result (rows in `data`, column names in
// SELECT order in `columns`) and writes it as JSON text with the shape
// applied.
//
// It shapes the QueryResult, not JSON text (#275). Shaping text meant parsing
// it back into crow values: crow's rvalue sorts an object's keys on the first
// lookup and its wvalue object is an unordered map, so every shaped row came
// back in a different column order, and re-printing re-formatted every double
// with 21 significant digits (711.56 became 711.55999999999994543). Rows are
// written here the way QueryResult::dumpData() writes them: columns in SELECT
// order, doubles in their shortest round-trip form.
//
// The sample payload is `{"sampled":true,"row_count":N,"columns":[...]}`, in
// that key order. A streamed result (rows already sent to a ResultSink as
// text) cannot be redacted or capped and is refused with std::logic_error
// rather than returned unshaped.
class MCPResponseShaper {
public:
    static constexpr const char* kRedactedSentinel = "<redacted>";

    std::string shape(const QueryResult& result, const ResponseShape& config) const;
};

} // namespace flapi
