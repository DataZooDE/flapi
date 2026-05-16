#include "mcp_response_shaper.hpp"

#include <algorithm>
#include <crow/json.h>
#include <set>

namespace flapi {

namespace {

bool isRedacted(const std::set<std::string>& names, const std::string& key) {
    return names.find(key) != names.end();
}

crow::json::wvalue cloneRvalue(const crow::json::rvalue& source) {
    return crow::json::wvalue(source);
}

crow::json::wvalue redactRow(const crow::json::rvalue& row,
                             const std::set<std::string>& redact_set) {
    if (row.t() != crow::json::type::Object) {
        return cloneRvalue(row);
    }
    crow::json::wvalue out;
    for (const auto& key : row.keys()) {
        if (isRedacted(redact_set, key)) {
            out[key] = MCPResponseShaper::kRedactedSentinel;
        } else {
            out[key] = row[key];
        }
    }
    return out;
}

std::vector<std::string> collectColumnNames(const crow::json::rvalue& array) {
    std::vector<std::string> names;
    if (array.t() != crow::json::type::List || array.size() == 0) {
        return names;
    }
    const auto& first = array[0];
    if (first.t() != crow::json::type::Object) {
        return names;
    }
    for (const auto& key : first.keys()) {
        names.push_back(key);
    }
    return names;
}

std::string buildSamplePayload(const crow::json::rvalue& array) {
    crow::json::wvalue out;
    out["sampled"] = true;
    out["row_count"] = static_cast<int64_t>(array.size());
    crow::json::wvalue columns = crow::json::wvalue::list();
    auto names = collectColumnNames(array);
    for (size_t i = 0; i < names.size(); ++i) {
        columns[i] = names[i];
    }
    out["columns"] = std::move(columns);
    return out.dump();
}

} // namespace

std::string MCPResponseShaper::shape(const std::string& json_payload,
                                     const ResponseShape& config) const {
    if (config.isNoOp()) {
        return json_payload;
    }

    auto parsed = crow::json::load(json_payload);
    if (!parsed || parsed.t() != crow::json::type::List) {
        // Non-array payloads (objects, primitives, malformed JSON) pass
        // through unchanged — we don't impose row-shape semantics on them.
        return json_payload;
    }

    if (config.sample) {
        return buildSamplePayload(parsed);
    }

    const std::set<std::string> redact_set(
        config.redact_columns.begin(), config.redact_columns.end());

    const size_t cap = config.max_rows.has_value() ? *config.max_rows : parsed.size();
    const size_t emit_count = std::min<size_t>(cap, parsed.size());

    crow::json::wvalue out = crow::json::wvalue::list();
    for (size_t i = 0; i < emit_count; ++i) {
        if (redact_set.empty()) {
            out[i] = parsed[i];
        } else {
            out[i] = redactRow(parsed[i], redact_set);
        }
    }
    return out.dump();
}

} // namespace flapi
