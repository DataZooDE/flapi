#include "sql_parameter_classifier.hpp"

#include "config_manager.hpp"

namespace flapi {

namespace {

// Returns `true` and sets `out` when `type_name` maps to a bindable
// SqlParameterType; otherwise returns `false` and leaves `out` untouched.
// Comparison is case-sensitive on purpose — see the test rationale.
bool tryMapType(const std::string& type_name, SqlParameterType& out) {
    if (type_name == "int" || type_name == "integer") {
        out = SqlParameterType::Integer;
        return true;
    }
    if (type_name == "number" || type_name == "float" || type_name == "double") {
        out = SqlParameterType::Double;
        return true;
    }
    if (type_name == "boolean" || type_name == "bool") {
        out = SqlParameterType::Boolean;
        return true;
    }
    if (type_name == "date") {
        out = SqlParameterType::Date;
        return true;
    }
    if (type_name == "time") {
        out = SqlParameterType::Time;
        return true;
    }
    if (type_name == "uuid" || type_name == "string" || type_name == "email" ||
        type_name == "enum") {
        out = SqlParameterType::Varchar;
        return true;
    }
    return false;
}

} // namespace

Bindability SqlParameterClassifier::classify(const RequestFieldConfig& field) const {
    Bindability result;
    for (const auto& validator : field.validators) {
        SqlParameterType mapped = SqlParameterType::Varchar;
        if (tryMapType(validator.type, mapped)) {
            result.bindable = true;
            result.type = mapped;
            return result;  // first known type wins, for determinism
        }
    }
    return result;  // bindable stays false
}

} // namespace flapi
