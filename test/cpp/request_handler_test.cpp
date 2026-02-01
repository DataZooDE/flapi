#include <catch2/catch_test_macros.hpp>
// Pre-include STL headers before the private-to-public hack
// to prevent "redeclared with different access" GCC errors
// when these headers are later included via crow/asio
#include <sstream>
#include <any>
#include <string>
#include <vector>
#include <map>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <optional>
#define private public
#include "../../src/include/request_handler.hpp"
#undef private
#include "../../src/include/config_manager.hpp"
#include <crow.h>

#include "test_utils.hpp"

using namespace flapi;
using namespace flapi::test;

namespace {

EndpointConfig createWriteEndpoint() {
    EndpointConfig endpoint;
    endpoint.urlPath = "/test";
    endpoint.method = "POST";
    endpoint.operation.type = OperationConfig::Write;
    endpoint.operation.transaction = true;
    endpoint.operation.validate_before_write = true;

    RequestFieldConfig nameField;
    nameField.fieldName = "name";
    nameField.fieldIn = "body";
    nameField.description = "Name field";
    nameField.required = true;
    endpoint.request_fields.push_back(nameField);

    RequestFieldConfig emailField;
    emailField.fieldName = "email";
    emailField.fieldIn = "body";
    emailField.description = "Email field";
    emailField.required = false;
    emailField.defaultValue = "default@example.com";
    endpoint.request_fields.push_back(emailField);

    return endpoint;
}

} // namespace

TEST_CASE("RequestHandler: combineWriteParameters merges sources with precedence", "[request_handler]") {
    TempTestConfig temp("request_handler_merge");
    auto config_manager = temp.createConfigManager();
    std::shared_ptr<DatabaseManager> db_manager; // not needed for parameter merging
    RequestHandler handler(db_manager, config_manager);

    EndpointConfig endpoint = createWriteEndpoint();

    crow::request req;
    req.method = crow::HTTPMethod::Post;
    req.body = R"({"name": "Body Name", "email": "body@example.com"})";
    req.url_params = std::string("?email=query@example.com&limit=50");

    std::map<std::string, std::string> pathParams = {{"name", "PathName"}, {"ignored", "value"}};

    auto params = handler.combineWriteParameters(req, pathParams, endpoint);

    REQUIRE(params.at("name") == "Body Name"); // body overrides path parameters
    REQUIRE(params.at("email") == "body@example.com"); // body overrides query parameter
    REQUIRE(params.at("offset") == "0"); // default from handler
    REQUIRE(params.at("limit") == "100"); // default preserved (query limit ignored)
    REQUIRE(params.at("ignored") == "value"); // path param retained when not overridden
}

TEST_CASE("RequestHandler: combineWriteParameters applies defaults for missing fields", "[request_handler]") {
    TempTestConfig temp("request_handler_defaults");
    auto config_manager = temp.createConfigManager();
    RequestHandler handler(nullptr, config_manager);
    EndpointConfig endpoint = createWriteEndpoint();

    crow::request req;
    req.method = crow::HTTPMethod::Post;
    req.body = R"({"name": "John"})";

    std::map<std::string, std::string> pathParams;
    auto params = handler.combineWriteParameters(req, pathParams, endpoint);

    REQUIRE(params.at("name") == "John");
    REQUIRE(params.at("email") == "default@example.com"); // default applied
}

TEST_CASE("RequestHandler: combineWriteParameters serializes complex JSON bodies", "[request_handler]") {
    TempTestConfig temp("request_handler_json");
    auto config_manager = temp.createConfigManager();
    RequestHandler handler(nullptr, config_manager);

    EndpointConfig endpoint = createWriteEndpoint();

    crow::request req;
    req.method = crow::HTTPMethod::Post;
    req.body = R"({
        "name": "Jane",
        "metadata": {"age": 30, "active": true},
        "tags": ["alpha", "beta"],
        "nickname": null
    })";

    std::map<std::string, std::string> pathParams;
    auto params = handler.combineWriteParameters(req, pathParams, endpoint);

    REQUIRE(params.at("name") == "Jane");

    auto metadataJson = crow::json::load(params.at("metadata"));
    REQUIRE(metadataJson["age"].i() == 30);
    REQUIRE(metadataJson["active"].b());

    auto tagsJson = crow::json::load(params.at("tags"));
    REQUIRE(tagsJson.size() == 2);
    REQUIRE(tagsJson[0].s() == "alpha");
    REQUIRE(tagsJson[1].s() == "beta");

    REQUIRE(params.find("nickname") != params.end());
    REQUIRE(params.at("nickname").empty()); // null serialized as empty string placeholder
}

TEST_CASE("RequestHandler: combineWriteParameters incorporates query parameters when body missing", "[request_handler]") {
    TempTestConfig temp("request_handler_query");
    auto config_manager = temp.createConfigManager();
    RequestHandler handler(nullptr, config_manager);
    EndpointConfig endpoint = createWriteEndpoint();

    crow::request req;
    req.method = crow::HTTPMethod::Post;
    req.body = R"({"name": "Query Backfill"})";
    req.url_params = std::string("?status=active&email=query@example.com");

    std::map<std::string, std::string> pathParams;
    auto params = handler.combineWriteParameters(req, pathParams, endpoint);

    REQUIRE(params.at("status") == "active");
    // Query email should not override default because body missing but default exists
    REQUIRE(params.at("email") == "default@example.com");
}

TEST_CASE("RequestHandler: combineWriteParameters preserves empty-string body values", "[request_handler]") {
    TempTestConfig temp("request_handler_empty_strings");
    auto config_manager = temp.createConfigManager();
    RequestHandler handler(nullptr, config_manager);

    EndpointConfig endpoint;
    endpoint.urlPath = "/test";
    endpoint.method = "PUT";
    endpoint.operation.type = OperationConfig::Write;

    // Define fields that can be empty strings
    RequestFieldConfig nameField;
    nameField.fieldName = "name";
    nameField.fieldIn = "body";
    nameField.required = false;
    endpoint.request_fields.push_back(nameField);

    RequestFieldConfig descField;
    descField.fieldName = "description";
    descField.fieldIn = "body";
    descField.required = false;
    endpoint.request_fields.push_back(descField);

    RequestFieldConfig statusField;
    statusField.fieldName = "status";
    statusField.fieldIn = "body";
    statusField.required = false;
    endpoint.request_fields.push_back(statusField);

    SECTION("Empty string from JSON body is preserved") {
        crow::request req;
        req.method = crow::HTTPMethod::Put;
        req.body = R"({"name": "", "description": "Some description"})";

        std::map<std::string, std::string> pathParams;
        auto params = handler.combineWriteParameters(req, pathParams, endpoint);

        // Empty string should be preserved, not treated as missing
        REQUIRE(params.find("name") != params.end());
        REQUIRE(params.at("name") == "");
        REQUIRE(params.at("description") == "Some description");
    }

    SECTION("Multiple empty strings are all preserved") {
        crow::request req;
        req.method = crow::HTTPMethod::Put;
        req.body = R"({"name": "", "description": "", "status": ""})";

        std::map<std::string, std::string> pathParams;
        auto params = handler.combineWriteParameters(req, pathParams, endpoint);

        REQUIRE(params.at("name") == "");
        REQUIRE(params.at("description") == "");
        REQUIRE(params.at("status") == "");
    }

    SECTION("Empty string is different from missing field") {
        crow::request req;
        req.method = crow::HTTPMethod::Put;
        req.body = R"({"name": ""})";  // description not present at all

        std::map<std::string, std::string> pathParams;
        auto params = handler.combineWriteParameters(req, pathParams, endpoint);

        // name should be present with empty value
        REQUIRE(params.find("name") != params.end());
        REQUIRE(params.at("name") == "");

        // description was never sent in the body
        // It might not be in params at all, or it might be from defaults
        // The key point is that empty string from body is preserved as empty
    }

    SECTION("Empty string body value triggers validation error if required") {
        RequestValidator validator;

        RequestFieldConfig requiredField;
        requiredField.fieldName = "required_name";
        requiredField.fieldIn = "body";
        requiredField.required = true;

        ValidatorConfig stringValidator;
        stringValidator.type = "string";
        stringValidator.min = 1;  // Require at least 1 character
        stringValidator.preventSqlInjection = false;
        requiredField.validators.push_back(stringValidator);

        std::vector<RequestFieldConfig> fields = {requiredField};

        // Test that empty string fails validation when min length is set
        std::map<std::string, std::string> params = {{"required_name", ""}};
        auto errors = validator.validateRequestParameters(fields, params);

        REQUIRE(errors.size() == 1);
        REQUIRE(errors[0].fieldName == "required_name");
        REQUIRE(errors[0].errorMessage == "String is shorter than the minimum allowed length");
    }

    SECTION("Whitespace-only string is preserved (not trimmed)") {
        crow::request req;
        req.method = crow::HTTPMethod::Put;
        req.body = R"({"name": "   ", "description": "\t\n"})";

        std::map<std::string, std::string> pathParams;
        auto params = handler.combineWriteParameters(req, pathParams, endpoint);

        // Whitespace-only strings should be preserved as-is
        REQUIRE(params.at("name") == "   ");
        REQUIRE(params.at("description") == "\t\n");
    }
}
