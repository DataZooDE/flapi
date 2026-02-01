#include "request_validator.hpp"
#include <regex>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <unordered_set>

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
            errors.push_back({field.fieldName, "Required field is missing"});
        }
        return errors;
    }

    const std::string& value = it->second;

    // Track if SQL injection validation should be performed
    // Default is true (ValidatorConfig defaults preventSqlInjection to true)
    // Only skip if ALL validators explicitly disable it (preventSqlInjection = false)
    bool shouldValidateSqlInjection = true;
    if (!field.validators.empty()) {
        // Check if ALL validators have preventSqlInjection = false
        bool allDisable = true;
        for (const auto& validator : field.validators) {
            if (validator.preventSqlInjection) {
                allDisable = false;
                break;
            }
        }
        shouldValidateSqlInjection = !allDisable;
    }
    
    for (const auto& validator : field.validators) {
        if (validator.type == "string") {
            auto stringErrors = validateString(field.fieldName, value, validator);
            errors.insert(errors.end(), stringErrors.begin(), stringErrors.end());
        } else if (validator.type == "int") {
            auto intErrors = validateInt(field.fieldName, value, validator);
            errors.insert(errors.end(), intErrors.begin(), intErrors.end());
        } else if (validator.type == "email") {
            auto emailErrors = validateEmail(field.fieldName, value);
            errors.insert(errors.end(), emailErrors.begin(), emailErrors.end());
        } else if (validator.type == "uuid") {
            auto uuidErrors = validateUUID(field.fieldName, value);
            errors.insert(errors.end(), uuidErrors.begin(), uuidErrors.end());
        } else if (validator.type == "date") {
            auto dateErrors = validateDate(field.fieldName, value, validator);
            errors.insert(errors.end(), dateErrors.begin(), dateErrors.end());
        } else if (validator.type == "time") {
            auto timeErrors = validateTime(field.fieldName, value, validator);
            errors.insert(errors.end(), timeErrors.begin(), timeErrors.end());
        } else if (validator.type == "enum") {
            auto enumErrors = validateEnum(field.fieldName, value, validator);
            errors.insert(errors.end(), enumErrors.begin(), enumErrors.end());
        }
    }

    // Perform SQL injection validation if enabled
    if (shouldValidateSqlInjection) {
        auto sqlInjectionErrors = validateSqlInjection(field.fieldName, value);
        errors.insert(errors.end(), sqlInjectionErrors.begin(), sqlInjectionErrors.end());
    }

    return errors;
}

std::vector<ValidationError> RequestValidator::validateString(const std::string& fieldName, const std::string& value, const ValidatorConfig& validator) {
    std::vector<ValidationError> errors;
    if (validator.min > 0 && static_cast<int>(value.size()) < validator.min) {
        errors.push_back({fieldName, "String is shorter than the minimum allowed length"});
    }
    if (validator.max > 0 && static_cast<int>(value.size()) > validator.max) {
        errors.push_back({fieldName, "String is longer than the maximum allowed length"});
    }
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
        if (intValue < validator.min) {
            errors.push_back({fieldName, "Integer is less than the minimum allowed value"});
        }
        if (intValue > validator.max) {
            errors.push_back({fieldName, "Integer is greater than the maximum allowed value"});
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
    
    // List of SQL keywords to check for (using whole-word matching)
    const std::vector<std::string> sqlKeywords = {
        "SELECT", "INSERT", "UPDATE", "DELETE", "DROP", "TRUNCATE", "ALTER", "CREATE", "TABLE",
        "UNION", "EXEC", "EXECUTE", "SCRIPT", "DECLARE", "CAST", "CONVERT"
    };

    // Use whole-word matching with regex to avoid false positives
    // e.g., "UPDATED" should not match "UPDATE", but "UPDATE test" should match
    for (const auto& keyword : sqlKeywords) {
        // Use word boundary regex to match whole words only
        std::string pattern = "\\b" + keyword + "\\b";
        std::regex keywordRegex(pattern, std::regex::icase);
        if (std::regex_search(value, keywordRegex)) {
            errors.push_back({fieldName, "Potential SQL injection detected"});
            return errors; // Return immediately if any keyword is found
        }
    }

    // Check for dangerous SQL injection patterns (more specific than just characters)
    // Patterns that are commonly used in SQL injection attacks
    const std::vector<std::string> dangerousPatterns = {
        "';",          // SQL injection termination
        "--",          // SQL comment
        "/*",          // SQL comment start
        "*/",          // SQL comment end
        "xp_",         // SQL Server extended procedure
        "sp_",         // SQL Server stored procedure
        " OR 1=1",     // SQL injection pattern (with leading space for whole phrase)
        " OR '1'='1",  // SQL injection pattern with quotes
        "AND 1=1",     // SQL injection pattern
        "1=1",         // Always true condition (as substring)
        "1=2"          // Always false condition (as substring)
    };
    
    std::string upperValue = value;
    std::transform(upperValue.begin(), upperValue.end(), upperValue.begin(), ::toupper);
    
    for (const auto& pattern : dangerousPatterns) {
        std::string upperPattern = pattern;
        std::transform(upperPattern.begin(), upperPattern.end(), upperPattern.begin(), ::toupper);
        if (upperValue.find(upperPattern) != std::string::npos) {
            errors.push_back({fieldName, "Potential SQL injection detected"});
            return errors; // Return immediately if any dangerous pattern is found
        }
    }

    // Check for suspicious characters that are commonly used in SQL injection
    // Single quotes are a key indicator of SQL injection attempts
    const std::string suspiciousChars = "\'";
    for (char c : suspiciousChars) {
        if (value.find(c) != std::string::npos) {
            // Check if it's part of a dangerous pattern (already checked above)
            // Only flag if it appears in a context that suggests injection
            // Look for patterns like ' OR ' or '; or similar
            bool isPartOfPattern = false;
            size_t pos = value.find(c);
            while (pos != std::string::npos) {
                // Check context around the quote
                std::string context = "";
                if (pos > 0 && pos < value.length() - 1) {
                    context = value.substr(std::max(0, (int)pos - 2), std::min(5, (int)(value.length() - pos + 2)));
                } else if (pos == 0 && value.length() > 1) {
                    context = value.substr(0, 3);
                } else if (pos == value.length() - 1 && value.length() > 1) {
                    context = value.substr(std::max(0, (int)pos - 2), 3);
                }
                std::transform(context.begin(), context.end(), context.begin(), ::toupper);
                // If quote appears with OR, AND, or ; nearby, it's likely injection
                if (context.find("OR") != std::string::npos || 
                    context.find("AND") != std::string::npos || 
                    context.find(";") != std::string::npos ||
                    context.find("=") != std::string::npos) {
                    isPartOfPattern = true;
                    break;
                }
                pos = value.find(c, pos + 1);
            }
            if (isPartOfPattern) {
                errors.push_back({fieldName, "Potential SQL injection detected"});
                return errors;
            }
        }
    }

    return errors;
}

std::vector<ValidationError> RequestValidator::validateRequestFields(
    const std::vector<RequestFieldConfig>& requestFields,
    const std::map<std::string, std::string>& params) {
    
    std::vector<ValidationError> errors;
    
    // Create a set of known field names for O(1) lookup
    std::unordered_set<std::string> knownFields;
    for (const auto& field : requestFields) {
        knownFields.insert(field.fieldName);
    }
    
    // Add pagination parameters as they are always allowed
    knownFields.insert("offset");
    knownFields.insert("limit");
    
    // Check each parameter against known fields
    for (const auto& param : params) {
        if (knownFields.find(param.first) == knownFields.end()) {
            errors.push_back({
                param.first,
                "Unknown parameter not defined in endpoint configuration"
            });
        }
    }
    
    return errors;
}

} // namespace flapi
