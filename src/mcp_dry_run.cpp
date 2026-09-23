#include "mcp_dry_run.hpp"

#include "redaction.hpp"

#include <algorithm>

namespace flapi {

// Below this length a value cannot be replaced without risking unrelated text.
constexpr std::size_t kMinScrubbableSecret = 4;


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
    //
    // Explicitly an OBJECT, even when nothing is left. A default-constructed
    // wvalue is Null, and `{"_dryRun": true}` - the minimal dry-run call -
    // strips to exactly that. applyDefaultArguments then calls has() on a
    // null, which throws "value is not a container", so a dry run of any
    // endpoint declaring a `default:` failed with an opaque error.
    crow::json::wvalue rebuilt = crow::json::wvalue::object();
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



const char* MCPDryRun::withheldPreview() {
    return "<preview withheld: a value this template can interpolate is a credential "
           "too short to redact reliably, and returning the rendered SQL would "
           "disclose it>";
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
        // A request field's configured `default:` is copied into params, so a
        // credential-valued default was handed straight back to an
        // unauthenticated _dryRun caller in this object - regardless of
        // whether the SQL itself was scrubbed.
        params_obj[k] = isCredentialKey(k) ? std::string("<redacted>") : v;
    }
    payload["parameters"] = std::move(params_obj);

    return payload.dump();
}

} // namespace flapi
