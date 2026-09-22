#include "mcp_dry_run.hpp"

#include "redaction.hpp"

namespace flapi {

bool MCPDryRun::extractFlag(crow::json::wvalue& arguments) {
    if (arguments.t() != crow::json::type::Object) {
        return false;
    }
    auto keys = arguments.keys();
    bool present = false;
    for (const auto& k : keys) {
        if (k == kFlagKey) {
            present = true;
            break;
        }
    }
    if (!present) {
        return false;
    }

    // We have to round-trip via the rvalue type to read the value back, since
    // crow::json::wvalue does not expose getters for individual children.
    auto dumped = arguments.dump();
    auto parsed = crow::json::load(dumped);
    bool flag_value = false;
    if (parsed && parsed.has(kFlagKey)) {
        const auto& node = parsed[kFlagKey];
        if (node.t() == crow::json::type::True) {
            flag_value = true;
        }
    }

    // Rebuild the wvalue without the reserved key so downstream validators
    // never observe `_dryRun` as an unknown parameter.
    crow::json::wvalue rebuilt;
    if (parsed) {
        for (const auto& key : parsed.keys()) {
            if (key == kFlagKey) {
                continue;
            }
            rebuilt[key] = parsed[key];
        }
    }
    arguments = std::move(rebuilt);
    return flag_value;
}

std::string MCPDryRun::scrubConnectionSecrets(
    std::string sql,
    const std::unordered_map<std::string, std::string>& connection_properties) {
    for (const auto& [key, value] : connection_properties) {
        // Only credential-named properties, and only values long enough that
        // replacing them cannot mangle unrelated SQL. A one-character
        // "password" is not worth corrupting the output for.
        if (value.size() < 4 || !isCredentialKey(key)) {
            continue;
        }
        std::string::size_type pos = 0;
        while ((pos = sql.find(value, pos)) != std::string::npos) {
            sql.replace(pos, value.size(), "<redacted>");
            pos += sizeof("<redacted>") - 1;
        }
    }
    return sql;
}

std::string MCPDryRun::formatResult(const std::string& tool_name,
                                    const std::string& rendered_sql,
                                    const std::map<std::string, std::string>& parameters) {
    crow::json::wvalue payload;
    payload["dry_run"] = true;
    payload["tool_name"] = tool_name;
    payload["rendered_sql"] = rendered_sql;

    crow::json::wvalue params_obj = crow::json::wvalue::object();
    for (const auto& [k, v] : parameters) {
        params_obj[k] = v;
    }
    payload["parameters"] = std::move(params_obj);

    return payload.dump();
}

} // namespace flapi
