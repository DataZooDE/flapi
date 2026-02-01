#include <catch2/catch_test_macros.hpp>

#include "request_validator.hpp"
#include "config_manager.hpp"

using namespace flapi;

TEST_CASE("RequestValidator: validateRequestParameters", "[request_validator]") {
    RequestValidator validator;
    
    RequestFieldConfig nameField = {};
    nameField.fieldName = "name";
    nameField.fieldIn = "query";
    nameField.description = "Name";
    nameField.required = true;

    ValidatorConfig stringValidator = {};
    stringValidator.type = "string";
    stringValidator.regex = "^[a-zA-Z]+$";
    nameField.validators.push_back(stringValidator);

    RequestFieldConfig ageField = {};
    ageField.fieldName = "age";
    ageField.fieldIn = "query";
    ageField.description = "Age";
    ageField.required = true;

    ValidatorConfig intValidator = {};
    intValidator.type = "int";
    intValidator.min = 0;
    intValidator.max = 120;
    ageField.validators.push_back(intValidator);

    RequestFieldConfig emailField = {};
    emailField.fieldName = "email";
    emailField.fieldIn = "query";
    emailField.description = "Email";
    emailField.required = false;
    ValidatorConfig emailValidator = {};
    emailValidator.type = "email";
    emailField.validators.push_back(emailValidator);

    RequestFieldConfig idField = {};
    idField.fieldName = "id";
    idField.fieldIn = "query";
    idField.description = "ID";
    idField.required = false;
    ValidatorConfig uuidValidator = {};
    uuidValidator.type = "uuid";
    idField.validators.push_back(uuidValidator);

    RequestFieldConfig dateField = {};
    dateField.fieldName = "date";
    dateField.fieldIn = "query";
    dateField.description = "Date";
    dateField.required = false;
    ValidatorConfig dateValidator = {};
    dateValidator.type = "date";
    dateValidator.minDate = "2000-01-01";
    dateValidator.maxDate = "2100-12-31";
    dateField.validators.push_back(dateValidator);

    RequestFieldConfig timeField = {};
    timeField.fieldName = "time";
    timeField.fieldIn = "query";
    timeField.description = "Time";
    timeField.required = false;
    ValidatorConfig timeValidator = {};
    timeValidator.type = "time";
    timeValidator.minTime = "09:00:00";
    timeValidator.maxTime = "17:00:00";
    timeField.validators.push_back(timeValidator);

    RequestFieldConfig statusField = {};
    statusField.fieldName = "status";
    statusField.fieldIn = "query";
    statusField.description = "Status";
    statusField.required = false;
    ValidatorConfig enumValidator = {};
    enumValidator.type = "enum";
    enumValidator.allowedValues = {"active", "inactive", "pending"};
    statusField.validators.push_back(enumValidator);

    std::vector<RequestFieldConfig> requestFields = {
        nameField,
        ageField,
        emailField,
        idField,
        dateField,
        timeField,
        statusField
    };

    SECTION("Valid parameters") {
        std::map<std::string, std::string> params = {
            {"name", "John"},
            {"age", "30"},
            {"email", "john@example.com"},
            {"id", "550e8400-e29b-41d4-a716-446655440000"},
            {"date", "2023-05-01"},
            {"time", "14:30:00"},
            {"status", "active"}
        };
        auto errors = validator.validateRequestParameters(requestFields, params);
        REQUIRE(errors.empty());
    }

    SECTION("Invalid parameters") {
        std::map<std::string, std::string> params = {
            {"name", "John123"},
            {"age", "150"},
            {"email", "invalid-email"},
            {"id", "invalid-uuid"},
            {"date", "2200-01-01"},
            {"time", "18:00:00"},
            {"status", "unknown"}
        };
        auto errors = validator.validateRequestParameters(requestFields, params);
        REQUIRE(errors.size() == 7);
    }

    SECTION("Missing required field") {
        std::map<std::string, std::string> params = {
            {"age", "30"}
        };
        auto errors = validator.validateRequestParameters(requestFields, params);
        REQUIRE(errors.size() == 1);
        REQUIRE(errors[0].fieldName == "name");
        REQUIRE(errors[0].errorMessage == "Required field is missing");
    }
}

TEST_CASE("RequestValidator: validateString", "[request_validator]") {
    RequestValidator validator;
    RequestFieldConfig field = {};
    field.fieldName = "name";
    field.fieldIn = "query";
    field.description = "Name";
    field.required = true;

    ValidatorConfig validatorConfig = {};
    validatorConfig.type = "string";
    validatorConfig.regex = "^[a-zA-Z]+$";
    field.validators.push_back(validatorConfig);

    SECTION("Valid string") {
        std::map<std::string, std::string> params = {{"name", "John"}};
        auto errors = validator.validateRequestParameters({field}, params);
        REQUIRE(errors.empty());
    }

    SECTION("Invalid string") {
        std::map<std::string, std::string> params = {{"name", "John123"}};
        auto errors = validator.validateRequestParameters({field}, params);
        REQUIRE(errors.size() == 1);
        REQUIRE(errors[0].fieldName == "name");
        REQUIRE(errors[0].errorMessage == "Invalid string format");
    }
}

TEST_CASE("RequestValidator: validateString min/max length", "[request_validator]") {
    RequestValidator validator;

    SECTION("String minimum length enforcement") {
        RequestFieldConfig field = {};
        field.fieldName = "username";
        field.fieldIn = "body";
        field.description = "Username";
        field.required = true;

        ValidatorConfig validatorConfig = {};
        validatorConfig.type = "string";
        validatorConfig.min = 3;  // Minimum 3 characters
        validatorConfig.preventSqlInjection = false;  // Disable SQL injection check for this test
        field.validators.push_back(validatorConfig);

        // Valid: exactly at minimum
        std::map<std::string, std::string> params = {{"username", "abc"}};
        auto errors = validator.validateRequestParameters({field}, params);
        REQUIRE(errors.empty());

        // Valid: above minimum
        params["username"] = "abcdef";
        errors = validator.validateRequestParameters({field}, params);
        REQUIRE(errors.empty());

        // Invalid: below minimum
        params["username"] = "ab";
        errors = validator.validateRequestParameters({field}, params);
        REQUIRE(errors.size() == 1);
        REQUIRE(errors[0].fieldName == "username");
        REQUIRE(errors[0].errorMessage == "String is shorter than the minimum allowed length");

        // Invalid: single character
        params["username"] = "a";
        errors = validator.validateRequestParameters({field}, params);
        REQUIRE(errors.size() == 1);
        REQUIRE(errors[0].errorMessage == "String is shorter than the minimum allowed length");
    }

    SECTION("String maximum length enforcement") {
        RequestFieldConfig field = {};
        field.fieldName = "bio";
        field.fieldIn = "body";
        field.description = "Bio";
        field.required = true;

        ValidatorConfig validatorConfig = {};
        validatorConfig.type = "string";
        validatorConfig.max = 10;  // Maximum 10 characters
        validatorConfig.preventSqlInjection = false;
        field.validators.push_back(validatorConfig);

        // Valid: exactly at maximum
        std::map<std::string, std::string> params = {{"bio", "1234567890"}};
        auto errors = validator.validateRequestParameters({field}, params);
        REQUIRE(errors.empty());

        // Valid: below maximum
        params["bio"] = "short";
        errors = validator.validateRequestParameters({field}, params);
        REQUIRE(errors.empty());

        // Invalid: exceeds maximum
        params["bio"] = "12345678901";
        errors = validator.validateRequestParameters({field}, params);
        REQUIRE(errors.size() == 1);
        REQUIRE(errors[0].fieldName == "bio");
        REQUIRE(errors[0].errorMessage == "String is longer than the maximum allowed length");

        // Invalid: significantly exceeds maximum
        params["bio"] = "this is a very long string that exceeds the limit";
        errors = validator.validateRequestParameters({field}, params);
        REQUIRE(errors.size() == 1);
        REQUIRE(errors[0].errorMessage == "String is longer than the maximum allowed length");
    }

    SECTION("String min and max length combined") {
        RequestFieldConfig field = {};
        field.fieldName = "password";
        field.fieldIn = "body";
        field.description = "Password";
        field.required = true;

        ValidatorConfig validatorConfig = {};
        validatorConfig.type = "string";
        validatorConfig.min = 8;   // Minimum 8 characters
        validatorConfig.max = 20;  // Maximum 20 characters
        validatorConfig.preventSqlInjection = false;
        field.validators.push_back(validatorConfig);

        // Valid: exactly at minimum
        std::map<std::string, std::string> params = {{"password", "12345678"}};
        auto errors = validator.validateRequestParameters({field}, params);
        REQUIRE(errors.empty());

        // Valid: exactly at maximum
        params["password"] = "12345678901234567890";
        errors = validator.validateRequestParameters({field}, params);
        REQUIRE(errors.empty());

        // Valid: in between
        params["password"] = "password123";
        errors = validator.validateRequestParameters({field}, params);
        REQUIRE(errors.empty());

        // Invalid: too short
        params["password"] = "short";
        errors = validator.validateRequestParameters({field}, params);
        REQUIRE(errors.size() == 1);
        REQUIRE(errors[0].errorMessage == "String is shorter than the minimum allowed length");

        // Invalid: too long
        params["password"] = "123456789012345678901";  // 21 chars
        errors = validator.validateRequestParameters({field}, params);
        REQUIRE(errors.size() == 1);
        REQUIRE(errors[0].errorMessage == "String is longer than the maximum allowed length");
    }

    SECTION("Empty string validation") {
        RequestFieldConfig field = {};
        field.fieldName = "name";
        field.fieldIn = "body";
        field.description = "Name";
        field.required = true;

        ValidatorConfig validatorConfig = {};
        validatorConfig.type = "string";
        validatorConfig.min = 1;  // Require at least 1 character
        validatorConfig.preventSqlInjection = false;
        field.validators.push_back(validatorConfig);

        // Empty string should fail min length validation
        std::map<std::string, std::string> params = {{"name", ""}};
        auto errors = validator.validateRequestParameters({field}, params);
        REQUIRE(errors.size() == 1);
        REQUIRE(errors[0].fieldName == "name");
        REQUIRE(errors[0].errorMessage == "String is shorter than the minimum allowed length");
    }

    SECTION("No length constraints (min=0, max=0)") {
        RequestFieldConfig field = {};
        field.fieldName = "notes";
        field.fieldIn = "body";
        field.description = "Notes";
        field.required = false;

        ValidatorConfig validatorConfig = {};
        validatorConfig.type = "string";
        // min and max default to 0, meaning no length constraints
        validatorConfig.preventSqlInjection = false;
        field.validators.push_back(validatorConfig);

        // Empty string should be allowed
        std::map<std::string, std::string> params = {{"notes", ""}};
        auto errors = validator.validateRequestParameters({field}, params);
        REQUIRE(errors.empty());

        // Very long string should be allowed
        params["notes"] = std::string(1000, 'x');  // 1000 character string
        errors = validator.validateRequestParameters({field}, params);
        REQUIRE(errors.empty());
    }
}

TEST_CASE("RequestValidator: validateInt", "[request_validator]") {
    RequestValidator validator;
    RequestFieldConfig field = {};
    field.fieldName = "age";
    field.fieldIn = "query";
    field.description = "Age";
    field.required = true;

    ValidatorConfig validatorConfig = {};
    validatorConfig.type = "int";
    validatorConfig.min = 0;
    validatorConfig.max = 120;
    field.validators.push_back(validatorConfig);
      
    SECTION("Valid integer") {
        std::map<std::string, std::string> params = {{"age", "30"}};
        auto errors = validator.validateRequestParameters({field}, params);
        REQUIRE(errors.empty());
    }

    SECTION("Invalid integer") {
        std::map<std::string, std::string> params = {{"age", "150"}};
        auto errors = validator.validateRequestParameters({field}, params);
        REQUIRE(errors.size() == 1);
        REQUIRE(errors[0].fieldName == "age");
        REQUIRE(errors[0].errorMessage == "Integer is greater than the maximum allowed value");
    }
}

TEST_CASE("RequestValidator: validateEmail", "[request_validator]") {
    RequestValidator validator;
    RequestFieldConfig field = {};
    field.fieldName = "email";
    field.fieldIn = "query";
    field.description = "Email";
    field.required = false;

    ValidatorConfig validatorConfig = {};
    validatorConfig.type = "email";
    field.validators.push_back(validatorConfig);

    SECTION("Valid email") {
        std::map<std::string, std::string> params = {{"email", "john@example.com"}};
        auto errors = validator.validateRequestParameters({field}, params);
        REQUIRE(errors.empty());
    }

    SECTION("Invalid email") {
        std::map<std::string, std::string> params = {{"email", "invalid-email"}};
        auto errors = validator.validateRequestParameters({field}, params);
        REQUIRE(errors.size() == 1);
        REQUIRE(errors[0].fieldName == "email");
        REQUIRE(errors[0].errorMessage == "Invalid email format");
    }
}

TEST_CASE("RequestValidator: validateUUID", "[request_validator]") {
    RequestValidator validator;
    RequestFieldConfig field = {};
    field.fieldName = "id";
    field.fieldIn = "query";
    field.description = "ID";
    field.required = false;

    ValidatorConfig validatorConfig = {};
    validatorConfig.type = "uuid";
    field.validators.push_back(validatorConfig);

    SECTION("Valid UUID") {
        std::map<std::string, std::string> params = {{"id", "550e8400-e29b-41d4-a716-446655440000"}};
        auto errors = validator.validateRequestParameters({field}, params);
        REQUIRE(errors.empty());
    }

    SECTION("Invalid UUID") {
        std::map<std::string, std::string> params = {{"id", "invalid-uuid"}};
        auto errors = validator.validateRequestParameters({field}, params);
        REQUIRE(errors.size() == 1);
        REQUIRE(errors[0].fieldName == "id");
        REQUIRE(errors[0].errorMessage == "Invalid UUID format");
    }
}

TEST_CASE("RequestValidator: validateDate", "[request_validator]") {
    RequestValidator validator;
    RequestFieldConfig field = {};
    field.fieldName = "date";
    field.fieldIn = "query";
    field.description = "Date";
    field.required = false;

    ValidatorConfig validatorConfig = {};
    validatorConfig.type = "date";
    validatorConfig.minDate = "2000-01-01";
    validatorConfig.maxDate = "2100-12-31";
    field.validators.push_back(validatorConfig);

    SECTION("Valid date") {
        std::map<std::string, std::string> params = {{"date", "2023-05-01"}};
        auto errors = validator.validateRequestParameters({field}, params);
        REQUIRE(errors.empty());
    }

    SECTION("Invalid date") {
        std::map<std::string, std::string> params = {{"date", "2200-01-01"}};
        auto errors = validator.validateRequestParameters({field}, params);
        REQUIRE(errors.size() == 1);
        REQUIRE(errors[0].fieldName == "date");
        REQUIRE(errors[0].errorMessage == "Date is after the maximum allowed date");
    }
}

TEST_CASE("RequestValidator: validateTime", "[request_validator]") {
    RequestValidator validator;
    RequestFieldConfig field = {};
    field.fieldName = "time";
    field.fieldIn = "query";
    field.description = "Time";
    field.required = false;

    ValidatorConfig validatorConfig = {};
    validatorConfig.type = "time";
    validatorConfig.minTime = "09:00:00";
    validatorConfig.maxTime = "17:00:00";
    field.validators.push_back(validatorConfig);

    SECTION("Valid time") {
        std::map<std::string, std::string> params = {{"time", "14:30:00"}};
        auto errors = validator.validateRequestParameters({field}, params);
        REQUIRE(errors.empty());
    }

    SECTION("Invalid time") {
        std::map<std::string, std::string> params = {{"time", "18:00:00"}};
        auto errors = validator.validateRequestParameters({field}, params);
        REQUIRE(errors.size() == 1);
        REQUIRE(errors[0].fieldName == "time");
        REQUIRE(errors[0].errorMessage == "Time is after the maximum allowed time");
    }
}

TEST_CASE("RequestValidator: validateEnum", "[request_validator]") {
    RequestValidator validator;
    RequestFieldConfig field = {};
    field.fieldName = "status";
    field.fieldIn = "query";
    field.description = "Status";
    field.required = false;

    ValidatorConfig validatorConfig = {};
    validatorConfig.type = "enum";
    validatorConfig.allowedValues = {"active", "inactive", "pending"};
    field.validators.push_back(validatorConfig);

    SECTION("Valid enum value") {
        std::map<std::string, std::string> params = {{"status", "active"}};
        auto errors = validator.validateRequestParameters({field}, params);
        REQUIRE(errors.empty());
    }

    SECTION("Invalid enum value") {
        std::map<std::string, std::string> params = {{"status", "unknown"}};
        auto errors = validator.validateRequestParameters({field}, params);
        REQUIRE(errors.size() == 1);
        REQUIRE(errors[0].fieldName == "status");
        REQUIRE(errors[0].errorMessage == "Invalid enum value");
    }
}

TEST_CASE("RequestValidator: validateSQLInjection", "[request_validator]") {
    RequestValidator validator;
    RequestFieldConfig field = {};
    field.fieldName = "query";
    field.fieldIn = "query";
    field.description = "Query";
    field.required = false;

    ValidatorConfig validatorConfig = {};
    validatorConfig.type = "string";
    field.validators.push_back(validatorConfig);

    SECTION("Valid query") {
        std::map<std::string, std::string> params = {{"query", "normal search query"}};
        auto errors = validator.validateRequestParameters({field}, params);
        REQUIRE(errors.empty());
    }

    SECTION("SQL injection attempt with keywords") {
        std::map<std::string, std::string> params = {{"query", "SELECT * FROM users"}};
        auto errors = validator.validateRequestParameters({field}, params);
        REQUIRE(errors.size() == 1);
        REQUIRE(errors[0].fieldName == "query");
        REQUIRE(errors[0].errorMessage == "Potential SQL injection detected");
    }

    SECTION("SQL injection attempt with suspicious characters") {
        std::map<std::string, std::string> params = {{"query", "user' OR '1'='1"}};
        auto errors = validator.validateRequestParameters({field}, params);
        REQUIRE(errors.size() == 1);
        REQUIRE(errors[0].fieldName == "query");
        REQUIRE(errors[0].errorMessage == "Potential SQL injection detected");
    }
    
    SECTION("SQL injection false positive: 'UPDATED' should not match 'UPDATE'") {
        // "UPDATED" contains "UPDATE" as substring, but should NOT trigger validation
        // with whole-word matching
        std::map<std::string, std::string> params = {{"query", "Alice Mutton Updated"}};
        auto errors = validator.validateRequestParameters({field}, params);
        REQUIRE(errors.empty());  // Should NOT trigger SQL injection error
    }
    
    SECTION("SQL injection detection: 'UPDATE test' should match as whole word") {
        // "UPDATE test" contains "UPDATE" as a whole word, should trigger validation
        std::map<std::string, std::string> params = {{"query", "UPDATE test"}};
        auto errors = validator.validateRequestParameters({field}, params);
        REQUIRE(errors.size() == 1);
        REQUIRE(errors[0].fieldName == "query");
        REQUIRE(errors[0].errorMessage == "Potential SQL injection detected");
    }
    
    SECTION("SQL injection detection: dangerous patterns like '1=1'") {
        std::map<std::string, std::string> params = {{"query", "test OR 1=1"}};
        auto errors = validator.validateRequestParameters({field}, params);
        REQUIRE(errors.size() == 1);
        REQUIRE(errors[0].fieldName == "query");
        REQUIRE(errors[0].errorMessage == "Potential SQL injection detected");
    }
    
    SECTION("SQL injection prevention disabled via flag") {
        // Create a field with preventSqlInjection = false
        RequestFieldConfig fieldWithDisabledValidation = {};
        fieldWithDisabledValidation.fieldName = "query";
        fieldWithDisabledValidation.fieldIn = "query";
        fieldWithDisabledValidation.description = "Query";
        fieldWithDisabledValidation.required = false;
        
        ValidatorConfig validatorConfigDisabled = {};
        validatorConfigDisabled.type = "string";
        validatorConfigDisabled.preventSqlInjection = false;  // Disable SQL injection validation
        fieldWithDisabledValidation.validators.push_back(validatorConfigDisabled);
        
        // Should NOT trigger SQL injection error even with dangerous content
        std::map<std::string, std::string> params = {{"query", "SELECT * FROM users"}};
        auto errors = validator.validateRequestParameters({fieldWithDisabledValidation}, params);
        REQUIRE(errors.empty());  // Should pass because validation is disabled
    }
}

TEST_CASE("RequestValidator: validateRequestFields", "[request_validator]") {
    RequestValidator validator;
    
    // Create some basic request fields for testing
    RequestFieldConfig nameField = {};
    nameField.fieldName = "name";
    nameField.fieldIn = "query";
    
    RequestFieldConfig ageField = {};
    ageField.fieldName = "age";
    ageField.fieldIn = "query";
    
    std::vector<RequestFieldConfig> requestFields = {nameField, ageField};

    SECTION("Valid parameters with only known fields") {
        std::map<std::string, std::string> params = {
            {"name", "John"},
            {"age", "30"},
            {"offset", "0"},  // Pagination params should be allowed
            {"limit", "100"}
        };
        auto errors = validator.validateRequestFields(requestFields, params);
        REQUIRE(errors.empty());
    }

    SECTION("Invalid parameters with unknown fields") {
        std::map<std::string, std::string> params = {
            {"name", "John"},
            {"age", "30"},
            {"unknown_param", "value"},
            {"another_unknown", "value2"}
        };
        auto errors = validator.validateRequestFields(requestFields, params);
        REQUIRE(errors.size() == 2);
        
        // Check that both expected error fields are present
        bool hasUnknownParam = false;
        bool hasAnotherUnknown = false;
        
        for (const auto& error : errors) {
            if (error.fieldName == "unknown_param") {
                hasUnknownParam = true;
                REQUIRE(error.errorMessage == "Unknown parameter not defined in endpoint configuration");
            }
            if (error.fieldName == "another_unknown") {
                hasAnotherUnknown = true;
                REQUIRE(error.errorMessage == "Unknown parameter not defined in endpoint configuration");
            }
        }
        
        REQUIRE(hasUnknownParam);
        REQUIRE(hasAnotherUnknown);
    }

    SECTION("Write operation validation: required fields enforced") {
        RequestFieldConfig requiredField;
        requiredField.fieldName = "name";
        requiredField.fieldIn = "body";
        requiredField.required = true;
        
        std::vector<RequestFieldConfig> writeFields = {requiredField};
        
        // Missing required field should generate error
        std::map<std::string, std::string> params = {};
        auto errors = validator.validateRequestParameters(writeFields, params);
        REQUIRE(errors.size() >= 1);
        bool hasRequiredError = false;
        for (const auto& error : errors) {
            if (error.fieldName == "name") {
                // Error message should be "Required field is missing"
                hasRequiredError = (error.errorMessage == "Required field is missing" || 
                                    error.errorMessage.find("Required") != std::string::npos ||
                                    error.errorMessage.find("required") != std::string::npos);
                if (hasRequiredError) break;
            }
        }
        REQUIRE(hasRequiredError);
        
        // With required field present, should pass
        params["name"] = "John Doe";
        errors = validator.validateRequestParameters(writeFields, params);
        REQUIRE(errors.empty());
    }
    
    SECTION("Write operation validation: unknown parameters rejected") {
        RequestFieldConfig knownField;
        knownField.fieldName = "name";
        knownField.fieldIn = "body";
        
        std::vector<RequestFieldConfig> writeFields = {knownField};
        
        // Unknown parameter should generate error when strict validation is enabled
        std::map<std::string, std::string> params = {
            {"name", "John"},
            {"unknown_field", "value"}
        };
        auto errors = validator.validateRequestFields(writeFields, params);
        REQUIRE(errors.size() >= 1);
        bool hasUnknownError = false;
        for (const auto& error : errors) {
            if (error.fieldName == "unknown_field") {
                hasUnknownError = true;
                REQUIRE(error.errorMessage.find("Unknown") != std::string::npos);
                break;
            }
        }
        REQUIRE(hasUnknownError);
    }

    SECTION("Empty parameters should be valid") {
        std::map<std::string, std::string> params = {};
        auto errors = validator.validateRequestFields(requestFields, params);
        REQUIRE(errors.empty());
    }

    SECTION("Only pagination parameters should be valid") {
        std::map<std::string, std::string> params = {
            {"offset", "0"},
            {"limit", "100"}
        };
        auto errors = validator.validateRequestFields(requestFields, params);
        REQUIRE(errors.empty());
    }

    SECTION("Mixed valid and invalid parameters") {
        std::map<std::string, std::string> params = {
            {"name", "John"},          // valid
            {"unknown_param", "123"},   // invalid
            {"offset", "0"},           // valid (pagination)
            {"invalid_field", "xyz"}    // invalid
        };
        auto errors = validator.validateRequestFields(requestFields, params);
        REQUIRE(errors.size() == 2);
        
        // Check that both expected error fields are present
        bool hasUnknownParam = false;
        bool hasInvalidField = false;
        
        for (const auto& error : errors) {
            if (error.fieldName == "unknown_param") {
                hasUnknownParam = true;
                REQUIRE(error.errorMessage == "Unknown parameter not defined in endpoint configuration");
            }
            if (error.fieldName == "invalid_field") {
                hasInvalidField = true;
                REQUIRE(error.errorMessage == "Unknown parameter not defined in endpoint configuration");
            }
        }
        
        REQUIRE(hasUnknownParam);
        REQUIRE(hasInvalidField);
    }
}