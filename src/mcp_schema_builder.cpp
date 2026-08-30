#include "mcp_schema_builder.hpp"

#include <limits>
#include <string>

#include "config_manager.hpp"

namespace flapi {

namespace {

// Numeric validators whose min/max were not set in YAML are filled with these
// sentinels (see ConfigManager). Emitting them as minimum/maximum would just be
// noise (the full int range), so they are suppressed.
constexpr int kIntUnsetMin = std::numeric_limits<int>::min();
constexpr int kIntUnsetMax = std::numeric_limits<int>::max();

bool hasLowerBound(int min) { return min != 0 && min != kIntUnsetMin; }
bool hasUpperBound(int max) { return max != 0 && max != kIntUnsetMax; }

// Apply a single validator's constraints onto a property object. Called once
// per validator on a field; later validators refine the same object, so the
// last type-bearing validator wins for `type`/`format` while numeric/string
// bounds accumulate. `type_out` records the JSON Schema type this validator
// implies (empty string for enum, which does not set a type on its own) so the
// caller can guarantee every property carries a `type`.
void applyValidator(crow::json::wvalue& prop, const ValidatorConfig& v, std::string& type_out) {
    const std::string& t = v.type;

    if (t == "int" || t == "integer") {
        type_out = "integer";
        if (hasLowerBound(v.min)) {
            prop["minimum"] = v.min;
        }
        if (hasUpperBound(v.max)) {
            prop["maximum"] = v.max;
        }
    } else if (t == "double" || t == "float" || t == "number") {
        type_out = "number";
        if (hasLowerBound(v.min)) {
            prop["minimum"] = v.min;
        }
        if (hasUpperBound(v.max)) {
            prop["maximum"] = v.max;
        }
    } else if (t == "boolean" || t == "bool") {
        type_out = "boolean";
    } else if (t == "date") {
        type_out = "string";
        prop["format"] = "date";
    } else if (t == "time") {
        type_out = "string";
        prop["format"] = "time";
    } else if (t == "uuid") {
        type_out = "string";
        prop["format"] = "uuid";
    } else if (t == "email") {
        type_out = "string";
        prop["format"] = "email";
    } else if (t == "enum") {
        // enum constrains values; leave the type to another validator, or the
        // string default the caller applies when no validator set a type.
        if (!v.allowedValues.empty()) {
            crow::json::wvalue values = crow::json::wvalue::list();
            for (size_t i = 0; i < v.allowedValues.size(); ++i) {
                values[i] = v.allowedValues[i];
            }
            prop["enum"] = std::move(values);
        }
    } else if (t == "string" || t.empty()) {
        type_out = "string";
        // For strings, min/max are interpreted as length bounds.
        if (v.min != 0) {
            prop["minLength"] = v.min;
        }
        if (v.max != 0) {
            prop["maxLength"] = v.max;
        }
    } else {
        // Unknown/custom validator type: fall back to string, keep description hints.
        type_out = "string";
    }

    // A regex constraint applies regardless of the base type (string pattern).
    if (!v.regex.empty()) {
        prop["pattern"] = v.regex;
    }
}

} // namespace

crow::json::wvalue MCPSchemaBuilder::buildInputSchema(const std::vector<RequestFieldConfig>& fields) {
    crow::json::wvalue schema;
    schema["type"] = "object";
    schema["properties"] = crow::json::wvalue::object();

    std::vector<std::string> required_fields;

    for (const auto& field : fields) {
        crow::json::wvalue prop;

        // Resolve the property type from the validators. An enum-only field, or
        // a field with no validators, defaults to string so every property is
        // guaranteed to carry a `type` (JSON Schema, and backward-compatible
        // with clients that expect one on every parameter).
        std::string resolved_type = "string";
        for (const auto& v : field.validators) {
            std::string vt;
            applyValidator(prop, v, vt);
            if (!vt.empty()) {
                resolved_type = vt;
            }
        }
        prop["type"] = resolved_type;

        // Always emit description (may be empty), matching the prior schema
        // shape so existing clients see the same fields.
        prop["description"] = field.description;
        if (!field.defaultValue.empty()) {
            prop["default"] = field.defaultValue;
        }

        if (field.required) {
            required_fields.push_back(field.fieldName);
        }

        schema["properties"][field.fieldName] = std::move(prop);
    }

    if (!required_fields.empty()) {
        schema["required"] = required_fields;
    }

    return schema;
}

} // namespace flapi
