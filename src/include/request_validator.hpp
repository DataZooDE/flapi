#pragma once

#include <vector>
#include <map>
#include <string>
#include <regex>
#include "config_manager.hpp"

namespace flapi {

struct ValidationError {
    std::string fieldName;
    std::string errorMessage;
};

class RequestValidator {
public:
    RequestValidator() = default;
    
    std::vector<ValidationError> validateRequestParameters(const std::vector<RequestFieldConfig>& requestFields, 
                                                           const std::map<std::string, std::string>& params);

private:
    std::vector<ValidationError> validateField(const RequestFieldConfig& field, const std::map<std::string, std::string>& params);
    std::vector<ValidationError> validateRequiredField(const RequestFieldConfig& field, const std::map<std::string, std::string>& params);

    // Updated validation methods
    std::vector<ValidationError> validateString(const std::string& fieldName, const std::string& value, const ValidatorConfig& validator);
    std::vector<ValidationError> validateInt(const std::string& fieldName, const std::string& value, const ValidatorConfig& validator);
    std::vector<ValidationError> validateEmail(const std::string& fieldName, const std::string& value);
    std::vector<ValidationError> validateUUID(const std::string& fieldName, const std::string& value);
    std::vector<ValidationError> validateDate(const std::string& fieldName, const std::string& value, const ValidatorConfig& validator);
    std::vector<ValidationError> validateTime(const std::string& fieldName, const std::string& value, const ValidatorConfig& validator);
    std::vector<ValidationError> validateEnum(const std::string& fieldName, const std::string& value, const ValidatorConfig& validator);
    
    // New SQL injection prevention validator
    std::vector<ValidationError> validateSqlInjection(const std::string& fieldName, const std::string& value);
};

} // namespace flapi