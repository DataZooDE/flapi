#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_all.hpp>
#include "json_utils.hpp"

using namespace flapi;

TEST_CASE("JsonUtils::extractString", "[json][utils]") {
    SECTION("Extracts string with quotes") {
        crow::json::rvalue json = crow::json::load(R"({"field":"value"})");
        std::string result = JsonUtils::extractString(json["field"]);
        REQUIRE(result == "value");
    }

    SECTION("Handles empty string") {
        crow::json::rvalue json = crow::json::load(R"({"field":""})");
        std::string result = JsonUtils::extractString(json["field"]);
        REQUIRE(result == "");
    }

    SECTION("Returns empty for non-string") {
        crow::json::rvalue json = crow::json::load(R"({"field":123})");
        std::string result = JsonUtils::extractString(json["field"]);
        REQUIRE(result == "");
    }

    SECTION("Handles string with special characters") {
        crow::json::rvalue json = crow::json::load(R"({"field":"hello\nworld"})");
        std::string result = JsonUtils::extractString(json["field"]);
        REQUIRE_FALSE(result.empty());
    }

    SECTION("Handles string with quotes inside") {
        crow::json::rvalue json = crow::json::load(R"({"field":"say \"hi\""})");
        std::string result = JsonUtils::extractString(json["field"]);
        REQUIRE(result.find("say") != std::string::npos);
    }
}

TEST_CASE("JsonUtils::extractOptionalString", "[json][utils]") {
    SECTION("Returns value when present") {
        crow::json::rvalue json = crow::json::load(R"({"key":"value"})");
        auto result = JsonUtils::extractOptionalString(json, "key");
        REQUIRE(result.has_value());
        REQUIRE(result.value() == "value");
    }

    SECTION("Returns nullopt when missing") {
        crow::json::rvalue json = crow::json::load(R"({})");
        auto result = JsonUtils::extractOptionalString(json, "key");
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("Returns nullopt for non-string type") {
        crow::json::rvalue json = crow::json::load(R"({"key":123})");
        auto result = JsonUtils::extractOptionalString(json, "key");
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("Handles multiple keys") {
        crow::json::rvalue json = crow::json::load(R"({"a":"1","b":"2"})");
        auto result_a = JsonUtils::extractOptionalString(json, "a");
        auto result_b = JsonUtils::extractOptionalString(json, "b");
        REQUIRE(result_a.value() == "1");
        REQUIRE(result_b.value() == "2");
    }
}

TEST_CASE("JsonUtils::extractRequiredString", "[json][utils]") {
    SECTION("Returns value when present") {
        crow::json::rvalue json = crow::json::load(R"({"required":"value"})");
        std::string result = JsonUtils::extractRequiredString(json, "required");
        REQUIRE(result == "value");
    }

    SECTION("Throws when missing") {
        crow::json::rvalue json = crow::json::load(R"({})");
        REQUIRE_THROWS_AS(
            JsonUtils::extractRequiredString(json, "missing"),
            std::runtime_error
        );
    }

    SECTION("Throws when wrong type") {
        crow::json::rvalue json = crow::json::load(R"({"field":123})");
        REQUIRE_THROWS_AS(
            JsonUtils::extractRequiredString(json, "field"),
            std::runtime_error
        );
    }

    SECTION("Uses custom error message") {
        crow::json::rvalue json = crow::json::load(R"({})");
        try {
            JsonUtils::extractRequiredString(json, "field", "Custom error");
        } catch (const std::runtime_error& e) {
            REQUIRE(std::string(e.what()).find("Custom error") != std::string::npos);
        }
    }
}

TEST_CASE("JsonUtils::extractInt", "[json][utils]") {
    SECTION("Extracts integer") {
        crow::json::rvalue json = crow::json::load(R"({"num":42})");
        auto result = JsonUtils::extractInt(json, "num");
        REQUIRE(result.has_value());
        REQUIRE(result.value() == 42);
    }

    SECTION("Returns nullopt for missing key") {
        crow::json::rvalue json = crow::json::load(R"({})");
        auto result = JsonUtils::extractInt(json, "num");
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("Returns nullopt for non-number") {
        crow::json::rvalue json = crow::json::load(R"({"num":"42"})");
        auto result = JsonUtils::extractInt(json, "num");
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("Handles negative numbers") {
        crow::json::rvalue json = crow::json::load(R"({"num":-100})");
        auto result = JsonUtils::extractInt(json, "num");
        REQUIRE(result.value() == -100);
    }

    SECTION("Handles zero") {
        crow::json::rvalue json = crow::json::load(R"({"num":0})");
        auto result = JsonUtils::extractInt(json, "num");
        REQUIRE(result.value() == 0);
    }
}

TEST_CASE("JsonUtils::extractDouble", "[json][utils]") {
    SECTION("Extracts double") {
        crow::json::rvalue json = crow::json::load(R"({"num":3.14})");
        auto result = JsonUtils::extractDouble(json, "num");
        REQUIRE(result.has_value());
        REQUIRE_THAT(result.value(), Catch::Matchers::WithinAbs(3.14, 0.001));
    }

    SECTION("Returns nullopt for missing key") {
        crow::json::rvalue json = crow::json::load(R"({})");
        auto result = JsonUtils::extractDouble(json, "num");
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("Returns nullopt for non-number") {
        crow::json::rvalue json = crow::json::load(R"({"num":"3.14"})");
        auto result = JsonUtils::extractDouble(json, "num");
        REQUIRE_FALSE(result.has_value());
    }
}

TEST_CASE("JsonUtils::extractBool", "[json][utils]") {
    SECTION("Extracts true") {
        crow::json::rvalue json = crow::json::load(R"({"flag":true})");
        auto result = JsonUtils::extractBool(json, "flag");
        REQUIRE(result.has_value());
        REQUIRE(result.value() == true);
    }

    SECTION("Extracts false") {
        crow::json::rvalue json = crow::json::load(R"({"flag":false})");
        auto result = JsonUtils::extractBool(json, "flag");
        REQUIRE(result.has_value());
        REQUIRE(result.value() == false);
    }

    SECTION("Returns nullopt for missing key") {
        crow::json::rvalue json = crow::json::load(R"({})");
        auto result = JsonUtils::extractBool(json, "flag");
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("Returns nullopt for non-boolean") {
        crow::json::rvalue json = crow::json::load(R"({"flag":"true"})");
        auto result = JsonUtils::extractBool(json, "flag");
        REQUIRE_FALSE(result.has_value());
    }
}

TEST_CASE("JsonUtils::createErrorResponse", "[json][utils]") {
    SECTION("Creates error with message") {
        auto response = JsonUtils::createErrorResponse(400, "Bad Request");
        // Convert to string to check the response structure
        std::string response_str = response.dump();
        REQUIRE(response_str.find("Bad Request") != std::string::npos);
        REQUIRE(response_str.find("error") != std::string::npos);
    }

    SECTION("Includes details when provided") {
        auto response = JsonUtils::createErrorResponse(400, "Error", "Details here");
        std::string response_str = response.dump();
        REQUIRE(response_str.find("Details here") != std::string::npos);
    }

    SECTION("Works with various status codes") {
        auto err401 = JsonUtils::createErrorResponse(401, "Unauthorized");
        auto err500 = JsonUtils::createErrorResponse(500, "Internal Server Error");

        std::string str401 = err401.dump();
        std::string str500 = err500.dump();

        REQUIRE(str401.find("Unauthorized") != std::string::npos);
        REQUIRE(str500.find("Internal Server Error") != std::string::npos);
    }
}

TEST_CASE("JsonUtils::createSuccessResponse", "[json][utils]") {
    SECTION("Creates success response") {
        crow::json::wvalue data;
        data["result"] = "success";

        auto response = JsonUtils::createSuccessResponse(std::move(data));
        std::string response_str = response.dump();
        REQUIRE(response_str.find("success") != std::string::npos);
    }

    SECTION("Works with empty data") {
        crow::json::wvalue data;
        auto response = JsonUtils::createSuccessResponse(std::move(data));
        std::string response_str = response.dump();
        REQUIRE(response_str.find("success") != std::string::npos);
    }

    SECTION("Works with complex data") {
        crow::json::wvalue data;
        data["items"][0] = "item1";
        data["items"][1] = "item2";
        data["count"] = 2;

        auto response = JsonUtils::createSuccessResponse(std::move(data));
        std::string response_str = response.dump();
        REQUIRE(response_str.find("item1") != std::string::npos);
        REQUIRE(response_str.find("item2") != std::string::npos);
    }
}

TEST_CASE("JsonUtils type checking", "[json][utils]") {
    crow::json::rvalue json = crow::json::load(
        R"({"str":"text","num":42,"bool":true,"null":null,"arr":[1,2],"obj":{}})");

    SECTION("isString") {
        REQUIRE(JsonUtils::isString(json["str"]));
        REQUIRE_FALSE(JsonUtils::isString(json["num"]));
    }

    SECTION("isNumber") {
        REQUIRE(JsonUtils::isNumber(json["num"]));
        REQUIRE_FALSE(JsonUtils::isNumber(json["str"]));
    }

    SECTION("isBool") {
        REQUIRE(JsonUtils::isBool(json["bool"]));
        REQUIRE_FALSE(JsonUtils::isBool(json["num"]));
    }

    SECTION("isNull") {
        REQUIRE(JsonUtils::isNull(json["null"]));
        REQUIRE_FALSE(JsonUtils::isNull(json["str"]));
    }

    SECTION("isArray") {
        REQUIRE(JsonUtils::isArray(json["arr"]));
        REQUIRE_FALSE(JsonUtils::isArray(json["obj"]));
    }

    SECTION("isObject") {
        REQUIRE(JsonUtils::isObject(json["obj"]));
        REQUIRE_FALSE(JsonUtils::isObject(json["arr"]));
    }
}

TEST_CASE("JsonUtils::stringToJson", "[json][utils]") {
    SECTION("Converts string to JSON") {
        auto json = JsonUtils::stringToJson("test value");
        std::string json_str = json.dump();
        REQUIRE(json_str.find("test value") != std::string::npos);
    }

    SECTION("Handles empty string") {
        auto json = JsonUtils::stringToJson("");
        std::string json_str = json.dump();
        REQUIRE_FALSE(json_str.empty());
    }

    SECTION("Handles special characters") {
        auto json = JsonUtils::stringToJson("hello\nworld");
        std::string json_str = json.dump();
        REQUIRE_FALSE(json_str.empty());
    }
}

TEST_CASE("JsonUtils wvalue extractString overload", "[json][utils][wvalue]") {
    crow::json::wvalue json;
    json["text"] = "hello";
    json["number"] = 42;
    json["null_val"] = nullptr;

    SECTION("Extracts string from wvalue") {
        std::string result = JsonUtils::extractString(json["text"]);
        REQUIRE(result == "hello");
    }

    SECTION("Returns empty string for non-string wvalue") {
        std::string result = JsonUtils::extractString(json["number"]);
        REQUIRE(result == "");
    }

    SECTION("Returns empty string for null wvalue") {
        std::string result = JsonUtils::extractString(json["null_val"]);
        REQUIRE(result == "");
    }
}

TEST_CASE("JsonUtils wvalue extractOptionalString overload", "[json][utils][wvalue]") {
    crow::json::wvalue json;
    json["name"] = "Alice";
    json["age"] = 30;

    SECTION("Returns value for existing string") {
        auto result = JsonUtils::extractOptionalString(json, "name");
        REQUIRE(result.has_value());
        REQUIRE(result.value() == "Alice");
    }

    SECTION("Returns nullopt for missing key") {
        auto result = JsonUtils::extractOptionalString(json, "missing");
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("Returns nullopt for non-string value") {
        auto result = JsonUtils::extractOptionalString(json, "age");
        REQUIRE_FALSE(result.has_value());
    }
}

TEST_CASE("JsonUtils wvalue type checking overloads", "[json][utils][wvalue]") {
    crow::json::wvalue json;
    json["str"] = "text";
    json["num"] = 42;
    json["bool"] = true;
    json["null"] = nullptr;
    json["arr"][0] = 1;
    json["obj"]["key"] = "value";

    SECTION("isString on wvalue") {
        REQUIRE(JsonUtils::isString(json["str"]));
        REQUIRE_FALSE(JsonUtils::isString(json["num"]));
    }

    SECTION("isNumber on wvalue") {
        REQUIRE(JsonUtils::isNumber(json["num"]));
        REQUIRE_FALSE(JsonUtils::isNumber(json["str"]));
    }

    SECTION("isBool on wvalue") {
        REQUIRE(JsonUtils::isBool(json["bool"]));
        REQUIRE_FALSE(JsonUtils::isBool(json["num"]));
    }

    SECTION("isNull on wvalue") {
        REQUIRE(JsonUtils::isNull(json["null"]));
        REQUIRE_FALSE(JsonUtils::isNull(json["str"]));
    }

    SECTION("isArray on wvalue") {
        REQUIRE(JsonUtils::isArray(json["arr"]));
        REQUIRE_FALSE(JsonUtils::isArray(json["obj"]));
    }

    SECTION("isObject on wvalue") {
        REQUIRE(JsonUtils::isObject(json["obj"]));
        REQUIRE_FALSE(JsonUtils::isObject(json["arr"]));
    }
}

TEST_CASE("JsonUtils::valueToString with rvalue", "[json][utils][conversion]") {
    crow::json::rvalue json = crow::json::load(
        R"({"str":"hello","num":42,"float":3.14,"bool_true":true,"bool_false":false,"null_val":null})");

    SECTION("Converts string to string") {
        auto result = JsonUtils::valueToString(json["str"]);
        REQUIRE(result == "hello");
    }

    SECTION("Converts integer to string") {
        auto result = JsonUtils::valueToString(json["num"]);
        REQUIRE(result == "42");
    }

    SECTION("Converts double to string") {
        auto result = JsonUtils::valueToString(json["float"]);
        // Just verify it converts successfully and is not empty
        REQUIRE(!result.empty());
        // Should be a numeric string
        REQUIRE(std::isdigit(result[0]));
    }

    SECTION("Converts true to string") {
        auto result = JsonUtils::valueToString(json["bool_true"]);
        REQUIRE(result == "true");
    }

    SECTION("Converts false to string") {
        auto result = JsonUtils::valueToString(json["bool_false"]);
        REQUIRE(result == "false");
    }

    SECTION("Converts null to empty string") {
        auto result = JsonUtils::valueToString(json["null_val"]);
        REQUIRE(result == "");
    }
}

TEST_CASE("JsonUtils::valueToString with wvalue", "[json][utils][conversion][wvalue]") {
    crow::json::wvalue json;
    json["str"] = "world";
    json["num"] = 100;
    json["float"] = 2.71;
    json["bool_true"] = true;
    json["bool_false"] = false;
    json["null_val"] = nullptr;

    SECTION("Converts string to string") {
        auto result = JsonUtils::valueToString(json["str"]);
        REQUIRE(result == "world");
    }

    SECTION("Converts integer to string") {
        auto result = JsonUtils::valueToString(json["num"]);
        REQUIRE(result == "100");
    }

    SECTION("Converts double to string") {
        auto result = JsonUtils::valueToString(json["float"]);
        // Just verify it converts successfully and is not empty
        REQUIRE(!result.empty());
        // Should be a numeric string
        REQUIRE(std::isdigit(result[0]));
    }

    SECTION("Converts true to string") {
        auto result = JsonUtils::valueToString(json["bool_true"]);
        REQUIRE(result == "true");
    }

    SECTION("Converts false to string") {
        auto result = JsonUtils::valueToString(json["bool_false"]);
        REQUIRE(result == "false");
    }

    SECTION("Converts null to empty string") {
        auto result = JsonUtils::valueToString(json["null_val"]);
        REQUIRE(result == "");
    }
}
