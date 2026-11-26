#include "mcp_dry_run.hpp"

#include "redaction.hpp"
#include "auth_params.hpp"

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
        // The `__auth_*` keys are the identity the server injected for the
        // template's `auth.*`, not the caller's arguments: never echoed.
        if (isReservedAuthKey(k)) {
            continue;
        }
        // A request field's configured `default:` is copied into params, so a
        // credential-valued default was handed straight back to an
        // unauthenticated _dryRun caller in this object - regardless of
        // whether the SQL itself was scrubbed.
        params_obj[k] = isCredentialKey(k) ? std::string("<redacted>") : v;
    }
    payload["parameters"] = std::move(params_obj);

    return payload.dump();
}

namespace {

const char* typeName(SqlParameterType type) {
    switch (type) {
        case SqlParameterType::Integer: return "integer";
        case SqlParameterType::Double:  return "double";
        case SqlParameterType::Boolean: return "boolean";
        case SqlParameterType::Date:    return "date";
        case SqlParameterType::Time:    return "time";
        case SqlParameterType::Varchar: return "varchar";
    }
    return "varchar";
}

const RequestFieldConfig* findField(const std::vector<RequestFieldConfig>& fields, const std::string& name) {
    for (const auto& f : fields) {
        if (f.fieldName == name) {
            return &f;
        }
    }
    return nullptr;
}

}  // namespace

crow::json::wvalue MCPDryRun::typedParameter(const std::string& value, const RequestFieldConfig* field) {
    if (field != nullptr) {
        const auto bind = SqlParameterClassifier().classify(*field);
        try {
            std::size_t used = 0;
            switch (bind.type) {
                case SqlParameterType::Integer: {
                    const long long n = std::stoll(value, &used);
                    if (used == value.size()) {
                        return crow::json::wvalue(static_cast<int64_t>(n));
                    }
                    break;
                }
                case SqlParameterType::Double: {
                    const double d = std::stod(value, &used);
                    if (used == value.size()) {
                        return crow::json::wvalue(d);
                    }
                    break;
                }
                case SqlParameterType::Boolean:
                    if (value == "true" || value == "false") {
                        return crow::json::wvalue(value == "true");
                    }
                    break;
                default:
                    break;
            }
        } catch (const std::exception&) {
            // not a number after all: echoed as the string it is
        }
    }
    return crow::json::wvalue(value);
}

crow::json::wvalue MCPDryRun::bindingsJson(const std::vector<PreparedBindingSpec>& bindings,
                                           const std::map<std::string, std::string>& values,
                                           const std::vector<RequestFieldConfig>& fields,
                                           const TemplateSecrets& secrets) {
    crow::json::wvalue list = crow::json::wvalue::list();
    for (std::size_t i = 0; i < bindings.size(); ++i) {
        const auto& b = bindings[i];
        crow::json::wvalue entry;
        entry["position"] = static_cast<int64_t>(i + 1);
        entry["name"] = b.field_name;
        entry["type"] = typeName(b.type);
        const auto it = values.find(b.field_name);
        if (it == values.end()) {
            entry["value"] = nullptr;
        } else if (isCredentialKey(b.field_name)) {
            entry["value"] = "<redacted>";
        } else {
            const std::string scrubbed = secrets.withhold() ? std::string("<redacted>") : secrets.scrub(it->second);
            entry["value"] = scrubbed == it->second ? typedParameter(it->second, findField(fields, b.field_name))
                                                    : crow::json::wvalue(scrubbed);
        }
        list[i] = std::move(entry);
    }
    return list;
}

std::string MCPDryRun::formatResult(const std::string& tool_name,
                                    const std::string& rendered_sql,
                                    const std::map<std::string, std::string>& parameters,
                                    const std::vector<PreparedBindingSpec>& bindings,
                                    const std::vector<RequestFieldConfig>& fields,
                                    const TemplateSecrets& secrets) {
    crow::json::wvalue payload;
    payload["dry_run"] = true;
    payload["tool_name"] = tool_name;
    payload["rendered_sql"] = rendered_sql;
    payload["bindings"] = bindingsJson(bindings, parameters, fields, secrets);

    crow::json::wvalue params_obj = crow::json::wvalue::object();
    for (const auto& [k, v] : parameters) {
        if (isReservedAuthKey(k)) {
            continue;  // injected identity, not an argument (see above)
        }
        if (isCredentialKey(k)) {
            params_obj[k] = std::string("<redacted>");
        } else {
            params_obj[k] = typedParameter(v, findField(fields, k));
        }
    }
    payload["parameters"] = std::move(params_obj);
    return payload.dump();
}

} // namespace flapi
