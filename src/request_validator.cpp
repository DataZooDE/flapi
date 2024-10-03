#include "request_validator.hpp"
#include <crow.h>
#include <regex>
#include <chrono>
#include <iomanip>
#include <algorithm>

namespace flapi {

std::vector<ValidationError> RequestValidator::validateRequestParameters(const std::vector<RequestFieldConfig>& requestFields, 
                                                                         const std::map<std::string, std::string>& params) {
    std::vector<ValidationError> errors;
    for (const auto& field : requestFields) {
        auto fieldErrors = validateField(field, params);
        errors.insert(errors.end(), fieldErrors.begin(), fieldErrors.end());
    }
    return errors;
}

std::vector<ValidationError> RequestValidator::validateField(const RequestFieldConfig& field, const std::map<std::string, std::string>& params) {
    std::vector<ValidationError> errors;
    auto it = params.find(field.fieldName);
    if (it == params.end()) {
        if (field.required) {
            errors.push_back({field.fieldName, "Missing required parameter"});
        }
        return errors;
    }

    const std::string& value = it->second;

    for (const auto& validator : field.validators) {
        std::vector<ValidationError> validatorErrors;
        if (validator.type == "string") {
            validatorErrors = validateString(field.fieldName, value, validator);
        } else if (validator.type == "int") {
            validatorErrors = validateInt(field.fieldName, value, validator);
        } else if (validator.type == "email") {
            validatorErrors = validateEmail(field.fieldName, value);
        } else if (validator.type == "uuid") {
            validatorErrors = validateUUID(field.fieldName, value);
        } else if (validator.type == "date") {
            validatorErrors = validateDate(field.fieldName, value, validator);
        } else if (validator.type == "time") {
            validatorErrors = validateTime(field.fieldName, value, validator);
        } else if (validator.type == "enum") {
            validatorErrors = validateEnum(field.fieldName, value, validator);
        }
        errors.insert(errors.end(), validatorErrors.begin(), validatorErrors.end());
    }

    // Always check for SQL injection
    auto sqlInjectionErrors = validateSqlInjection(field.fieldName, value);
    errors.insert(errors.end(), sqlInjectionErrors.begin(), sqlInjectionErrors.end());

    return errors;
}

std::vector<ValidationError> RequestValidator::validateString(const std::string& fieldName, const std::string& value, const ValidatorConfig& validator) {
    std::vector<ValidationError> errors;
    if (!validator.regex.empty()) {
        std::regex pattern(validator.regex);
        if (!std::regex_match(value, pattern)) {
            errors.push_back({fieldName, "Invalid string format"});
        }
    }
    return errors;
}

std::vector<ValidationError> RequestValidator::validateInt(const std::string& fieldName, const std::string& value, const ValidatorConfig& validator) {
    std::vector<ValidationError> errors;
    try {
        int intValue = std::stoi(value);
        if (intValue < validator.min || intValue > validator.max) {
            errors.push_back({fieldName, "Integer value out of range"});
        }
    } catch (const std::exception&) {
        errors.push_back({fieldName, "Invalid integer value"});
    }
    return errors;
}

std::vector<ValidationError> RequestValidator::validateEmail(const std::string& fieldName, const std::string& value) {
    std::vector<ValidationError> errors;
    const std::regex pattern(R"([a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\.[a-zA-Z]{2,})");
    if (!std::regex_match(value, pattern)) {
        errors.push_back({fieldName, "Invalid email format"});
    }
    return errors;
}

std::vector<ValidationError> RequestValidator::validateUUID(const std::string& fieldName, const std::string& value) {
    std::vector<ValidationError> errors;
    const std::regex pattern("^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$");
    if (!std::regex_match(value, pattern)) {
        errors.push_back({fieldName, "Invalid UUID format"});
    }
    return errors;
}

std::vector<ValidationError> RequestValidator::validateDate(const std::string& fieldName, const std::string& value, const ValidatorConfig& validator) {
    std::vector<ValidationError> errors;
    std::tm tm = {};
    std::istringstream ss(value);
    ss >> std::get_time(&tm, "%Y-%m-%d");
    if (ss.fail()) {
        errors.push_back({fieldName, "Invalid date format"});
        return errors;
    }

    auto date = std::chrono::system_clock::from_time_t(std::mktime(&tm));

    if (!validator.minDate.empty()) {
        std::tm minTm = {};
        std::istringstream minSs(validator.minDate);
        minSs >> std::get_time(&minTm, "%Y-%m-%d");
        if (!minSs.fail() && date < std::chrono::system_clock::from_time_t(std::mktime(&minTm))) {
            errors.push_back({fieldName, "Date is before the minimum allowed date"});
        }
    }

    if (!validator.maxDate.empty()) {
        std::tm maxTm = {};
        std::istringstream maxSs(validator.maxDate);
        maxSs >> std::get_time(&maxTm, "%Y-%m-%d");
        if (!maxSs.fail() && date > std::chrono::system_clock::from_time_t(std::mktime(&maxTm))) {
            errors.push_back({fieldName, "Date is after the maximum allowed date"});
        }
    }

    return errors;
}

std::vector<ValidationError> RequestValidator::validateTime(const std::string& fieldName, const std::string& value, const ValidatorConfig& validator) {
    std::vector<ValidationError> errors;
    std::tm tm = {};
    std::istringstream ss(value);
    ss >> std::get_time(&tm, "%H:%M:%S");
    if (ss.fail()) {
        errors.push_back({fieldName, "Invalid time format"});
        return errors;
    }

    auto time = std::chrono::seconds(tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec);

    if (!validator.minTime.empty()) {
        std::tm minTm = {};
        std::istringstream minSs(validator.minTime);
        minSs >> std::get_time(&minTm, "%H:%M:%S");
        if (!minSs.fail() && time < std::chrono::seconds(minTm.tm_hour * 3600 + minTm.tm_min * 60 + minTm.tm_sec)) {
            errors.push_back({fieldName, "Time is before the minimum allowed time"});
        }
    }

    if (!validator.maxTime.empty()) {
        std::tm maxTm = {};
        std::istringstream maxSs(validator.maxTime);
        maxSs >> std::get_time(&maxTm, "%H:%M:%S");
        if (!maxSs.fail() && time > std::chrono::seconds(maxTm.tm_hour * 3600 + maxTm.tm_min * 60 + maxTm.tm_sec)) {
            errors.push_back({fieldName, "Time is after the maximum allowed time"});
        }
    }

    return errors;
}

std::vector<ValidationError> RequestValidator::validateEnum(const std::string& fieldName, const std::string& value, const ValidatorConfig& validator) {
    std::vector<ValidationError> errors;
    if (std::find(validator.allowedValues.begin(), validator.allowedValues.end(), value) == validator.allowedValues.end()) {
        errors.push_back({fieldName, "Invalid enum value"});
    }
    return errors;
}

std::vector<ValidationError> RequestValidator::validateSqlInjection(const std::string& fieldName, const std::string& value) {
    std::vector<ValidationError> errors;
    
    // List of SQL keywords and characters to check for
    const std::vector<std::string> sqlKeywords = {
        "SELECT", "INSERT", "UPDATE", "DELETE", "DROP", "TRUNCATE", "ALTER", "CREATE", "TABLE",
        "FROM", "WHERE", "AND", "OR", "UNION", "--", "/*", "*/", ";"
    };

    std::string upperValue = value;
    std::transform(upperValue.begin(), upperValue.end(), upperValue.begin(), ::toupper);

    for (const auto& keyword : sqlKeywords) {
        if (upperValue.find(keyword) != std::string::npos) {
            errors.push_back({fieldName, "Potential SQL injection detected"});
            return errors; // Return immediately if any keyword is found
        }
    }

    // Check for suspicious characters
    const std::string suspiciousChars = "\'\"=()";
    for (char c : suspiciousChars) {
        if (value.find(c) != std::string::npos) {
            errors.push_back({fieldName, "Potential SQL injection detected"});
            return errors; // Return immediately if any suspicious character is found
        }
    }

    return errors;
}

} // namespace flapi