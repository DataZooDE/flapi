#include "mcp_response_shaper.hpp"

#include <algorithm>
#include <crow/json.h>
#include <set>
#include <stdexcept>

#include "query_executor.hpp"

namespace flapi {

namespace {

// The keys of one row in the order they are written: the result's columns
// (SELECT order) when the row has exactly those keys, else the row's own.
// The same rule QueryResult::appendRow applies.
std::vector<std::string> rowKeys(const crow::json::wvalue& row, const std::vector<std::string>& columns) {
    std::vector<std::string> keys = row.keys();
    if (columns.size() != keys.size()) {
        return keys;
    }
    for (const auto& column : columns) {
        if (row.count(column) == 0) {
            return keys;
        }
    }
    return columns;
}

void appendString(const std::string& value, std::string& out) {
    out += '"';
    crow::json::escape(value, out);
    out += '"';
}

// One row as QueryResult::appendRow writes it, with the redacted columns'
// values replaced by the sentinel. Values are printed by QueryResult::dumpJson,
// so a double keeps its shortest round-trip text.
void appendShapedRow(const crow::json::wvalue& row, const std::vector<std::string>& columns,
                     const std::set<std::string>& redact_set, std::string& out) {
    if (redact_set.empty() || row.t() != crow::json::type::Object) {
        QueryResult::appendRow(row, columns, out);
        return;
    }
    out += '{';
    bool first = true;
    for (const auto& key : rowKeys(row, columns)) {
        if (!first) {
            out += ',';
        }
        first = false;
        appendString(key, out);
        out += ':';
        if (redact_set.count(key) > 0) {
            appendString(MCPResponseShaper::kRedactedSentinel, out);
        } else {
            out += QueryResult::dumpJson(row[key]);
        }
    }
    out += '}';
}

std::vector<std::string> columnNames(const QueryResult& result) {
    if (!result.columns.empty()) {
        return result.columns;
    }
    if (result.data.t() == crow::json::type::List && result.data.size() > 0) {
        const auto& first = result.data[0];
        if (first.t() == crow::json::type::Object) {
            return first.keys();
        }
    }
    return {};
}

std::string buildSamplePayload(const QueryResult& result) {
    // Written by hand, in this key order: a wvalue object is an unordered map.
    std::string out = "{\"sampled\":true,\"row_count\":";
    out += std::to_string(result.rowCount());
    out += ",\"columns\":[";
    bool first = true;
    for (const auto& name : columnNames(result)) {
        if (!first) {
            out += ',';
        }
        first = false;
        appendString(name, out);
    }
    out += "]}";
    return out;
}

} // namespace

std::string MCPResponseShaper::shape(const QueryResult& result, const ResponseShape& config) const {
    if (config.isNoOp()) {
        return result.dumpData();
    }
    if (config.sample) {
        return buildSamplePayload(result);
    }
    if (result.streamed) {
        // The rows exist only as text. Returning them unshaped would skip a
        // redaction the operator configured.
        throw std::logic_error("MCPResponseShaper: a streamed result cannot be shaped; "
                               "run the query without a ResultSink");
    }
    if (result.data.t() != crow::json::type::List) {
        return result.dumpData();
    }

    const std::set<std::string> redact_set(config.redact_columns.begin(), config.redact_columns.end());
    const std::size_t total = result.data.size();
    const std::size_t emit_count = config.max_rows.has_value() ? std::min(*config.max_rows, total) : total;

    std::string out = "[";
    for (std::size_t i = 0; i < emit_count; ++i) {
        if (i > 0) {
            out += ',';
        }
        appendShapedRow(result.data[static_cast<unsigned>(i)], result.columns, redact_set, out);
    }
    out += ']';
    return out;
}

} // namespace flapi
