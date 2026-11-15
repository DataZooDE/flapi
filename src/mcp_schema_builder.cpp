#include "mcp_schema_builder.hpp"

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

#include "config_manager.hpp"

namespace flapi {

namespace {

// Numeric validators whose min/max were not set in YAML are filled with these
// sentinels (see ConfigManager). Emitting them as minimum/maximum would just be
// noise (the full int range), so they are suppressed.
constexpr int kIntUnsetMin = std::numeric_limits<int>::min();
constexpr int kIntUnsetMax = std::numeric_limits<int>::max();

// A numeric bound is "set" iff it differs from the unset sentinel. Note we must
// NOT treat 0 as unset here: `min: 0` / `max: 0` are legitimate, meaningful
// bounds and must appear in the schema.
bool hasLowerBound(int min) { return min != kIntUnsetMin; }
bool hasUpperBound(int max) { return max != kIntUnsetMax; }

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
        // A number's bounds are doubles, parsed only when set. Reading the int
        // min/max here emitted their 0/0 struct defaults ("minimum":0,
        // "maximum":0) on every number, telling a model only 0 fits.
        if (v.minNumber) {
            prop["minimum"] = *v.minNumber;
        }
        if (v.maxNumber) {
            prop["maximum"] = *v.maxNumber;
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

// YAML hands every default over as a string, but a schema whose `default`
// does not match its own `type` ("default": "10" on an integer) is invalid
// JSON Schema, and clients that pre-fill arguments from it send a string the
// typed binder then has to coerce. Emit the default in the property's type; a
// value that does not parse as that type stays a string rather than being
// silently turned into something the author did not write.
void setTypedDefault(crow::json::wvalue& prop, const std::string& type, const std::string& value) {
    if (type == "integer") {
        try {
            std::size_t used = 0;
            const int64_t parsed = std::stoll(value, &used);
            if (used == value.size()) {
                prop["default"] = parsed;
                return;
            }
        } catch (const std::exception&) {
            // not an integer literal: keep the string form below
        }
    } else if (type == "number") {
        try {
            std::size_t used = 0;
            const double parsed = std::stod(value, &used);
            if (used == value.size()) {
                prop["default"] = parsed;
                return;
            }
        } catch (const std::exception&) {
            // not a number literal: keep the string form below
        }
    } else if (type == "boolean") {
        if (value == "true" || value == "false") {
            prop["default"] = (value == "true");
            return;
        }
    }
    prop["default"] = value;
}

} // namespace

crow::json::wvalue MCPSchemaBuilder::buildPropertySchema(const RequestFieldConfig& field) {
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
        setTypedDefault(prop, resolved_type, field.defaultValue);
    }
    // MCP 2026-07-28 x-mcp-header annotation: tells the client to mirror this
    // parameter into an Mcp-Param-<name> header for edge routing.
    if (!field.mcp_header.empty()) {
        prop["x-mcp-header"] = field.mcp_header;
    }
    return prop;
}

crow::json::wvalue MCPSchemaBuilder::buildInputSchema(const std::vector<RequestFieldConfig>& fields) {
    crow::json::wvalue schema;
    schema["type"] = "object";
    schema["properties"] = crow::json::wvalue::object();

    std::vector<std::string> required_fields;

    for (const auto& field : fields) {
        if (field.required) {
            required_fields.push_back(field.fieldName);
        }
        schema["properties"][field.fieldName] = buildPropertySchema(field);
    }

    if (!required_fields.empty()) {
        schema["required"] = required_fields;
    }

    return schema;
}

} // namespace flapi
