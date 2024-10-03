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
}