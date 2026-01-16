#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_all.hpp>
#include "error.hpp"

using namespace flapi;

TEST_CASE("Error construction", "[error]") {
    SECTION("Validation error") {
        auto err = Error::Validation("Invalid input", "Field 'id' must be numeric");
        REQUIRE(err.category == ErrorCategory::Validation);
        REQUIRE(err.http_status_code == 400);
        REQUIRE(err.message == "Invalid input");
        REQUIRE(err.details == "Field 'id' must be numeric");
    }

    SECTION("Database error") {
        auto err = Error::Database("Query failed", "Table 'users' not found");
        REQUIRE(err.category == ErrorCategory::Database);
        REQUIRE(err.http_status_code == 500);
        REQUIRE(err.message == "Query failed");
        REQUIRE(err.details == "Table 'users' not found");
    }

    SECTION("Configuration error") {
        auto err = Error::Config("Invalid config");
        REQUIRE(err.category == ErrorCategory::Configuration);
        REQUIRE(err.http_status_code == 500);
        REQUIRE(err.details.empty());
    }

    SECTION("Authentication error") {
        auto err = Error::Auth("Invalid token");
        REQUIRE(err.category == ErrorCategory::Authentication);
        REQUIRE(err.http_status_code == 401);
    }

    SECTION("Not found error") {
        auto err = Error::NotFound("Resource not found");
        REQUIRE(err.category == ErrorCategory::NotFound);
        REQUIRE(err.http_status_code == 404);
    }

    SECTION("Internal error") {
        auto err = Error::Internal("Unexpected error");
        REQUIRE(err.category == ErrorCategory::Internal);
        REQUIRE(err.http_status_code == 500);
    }
}

TEST_CASE("Error::getCategoryName", "[error]") {
    REQUIRE(Error::Validation("test").getCategoryName() == "Validation");
    REQUIRE(Error::Database("test").getCategoryName() == "Database");
    REQUIRE(Error::Config("test").getCategoryName() == "Configuration");
    REQUIRE(Error::Auth("test").getCategoryName() == "Authentication");
    REQUIRE(Error::NotFound("test").getCategoryName() == "NotFound");
    REQUIRE(Error::Internal("test").getCategoryName() == "Internal");
}

TEST_CASE("Error::toJson", "[error]") {
    SECTION("Error with details") {
        auto err = Error::Validation("Invalid input", "Must be positive");
        auto json = err.toJson();

        // Convert to string to verify structure
        std::string json_str = json.dump();
        REQUIRE(json_str.find("false") != std::string::npos);  // success: false
        REQUIRE(json_str.find("Validation") != std::string::npos);
        REQUIRE(json_str.find("Invalid input") != std::string::npos);
        REQUIRE(json_str.find("Must be positive") != std::string::npos);
    }

    SECTION("Error without details") {
        auto err = Error::Database("Query failed");
        auto json = err.toJson();

        std::string json_str = json.dump();
        REQUIRE(json_str.find("false") != std::string::npos);  // success: false
        REQUIRE(json_str.find("Database") != std::string::npos);
        REQUIRE(json_str.find("Query failed") != std::string::npos);
    }
}

TEST_CASE("Error::toHttpResponse", "[error]") {
    SECTION("Validation error response") {
        auto err = Error::Validation("Invalid input", "Field required");
        auto response = err.toHttpResponse();

        REQUIRE(response.code == 400);
        // Response body is JSON with error details
    }

    SECTION("Not found error response") {
        auto err = Error::NotFound("User not found");
        auto response = err.toHttpResponse();

        REQUIRE(response.code == 404);
    }

    SECTION("Database error response") {
        auto err = Error::Database("Query execution failed");
        auto response = err.toHttpResponse();

        REQUIRE(response.code == 500);
    }
}

TEST_CASE("Expected<T> - Success case", "[error]") {
    SECTION("Create with value") {
        Result<int> result(42);

        REQUIRE(result.has_value());
        REQUIRE(result.value() == 42);
        REQUIRE(*result == 42);
    }

    SECTION("Create with move") {
        std::string value = "test";
        Result<std::string> result(std::move(value));

        REQUIRE(result.has_value());
        REQUIRE(*result == "test");
    }

    SECTION("Boolean conversion") {
        Result<int> success(42);
        REQUIRE(static_cast<bool>(success));

        Result<int> failure(Error::Validation("test"));
        REQUIRE_FALSE(static_cast<bool>(failure));
    }

    SECTION("Dereference operators") {
        struct Data {
            int value;
            int getValue() const { return value; }
        };

        Data d{42};
        Result<Data> result(d);

        REQUIRE(result->getValue() == 42);
        REQUIRE((*result).value == 42);
    }
}

TEST_CASE("Expected<T> - Error case", "[error]") {
    SECTION("Create with error") {
        Result<int> result(Error::Validation("Invalid input"));

        REQUIRE_FALSE(result.has_value());
        REQUIRE(result.error().category == ErrorCategory::Validation);
        REQUIRE(result.error().message == "Invalid input");
    }

    SECTION("Accessing value from error throws") {
        Result<int> result(Error::Database("Query failed"));

        REQUIRE_THROWS_AS(result.value(), std::runtime_error);
    }

    SECTION("Accessing error from success throws") {
        Result<int> result(42);

        REQUIRE_THROWS_AS(result.error(), std::runtime_error);
    }

    SECTION("Boolean conversion for error") {
        Result<int> failure(Error::Internal("test"));
        REQUIRE_FALSE(static_cast<bool>(failure));
        REQUIRE_FALSE(failure.has_value());
    }
}

TEST_CASE("Expected<T> - Move semantics", "[error]") {
    SECTION("Move success value") {
        Result<std::string> r1("original");
        Result<std::string> r2 = std::move(r1);

        REQUIRE(r2.has_value());
        REQUIRE(*r2 == "original");
    }

    SECTION("Move error") {
        Result<int> r1(Error::Validation("test", "details"));
        Result<int> r2 = std::move(r1);

        REQUIRE_FALSE(r2.has_value());
        REQUIRE(r2.error().message == "test");
    }
}

TEST_CASE("Expected<T> - Pattern matching style usage", "[error]") {
    SECTION("Process success") {
        auto compute = [](int x) -> Result<int> {
            if (x < 0) {
                return Error::Validation("Value must be positive");
            }
            return x * 2;
        };

        auto result = compute(5);
        if (result.has_value()) {
            REQUIRE(result.value() == 10);
        } else {
            FAIL("Should have succeeded");
        }
    }

    SECTION("Process error") {
        auto compute = [](int x) -> Result<int> {
            if (x < 0) {
                return Error::Validation("Value must be positive");
            }
            return x * 2;
        };

        auto result = compute(-5);
        if (result.has_value()) {
            FAIL("Should have failed");
        } else {
            REQUIRE(result.error().category == ErrorCategory::Validation);
        }
    }
}

TEST_CASE("Expected<T> with complex types", "[error]") {
    SECTION("Vector of integers") {
        std::vector<int> vec{1, 2, 3};
        Result<std::vector<int>> result(vec);

        REQUIRE(result.has_value());
        REQUIRE(result.value().size() == 3);
    }

    SECTION("Struct") {
        struct Config {
            std::string name;
            int port;
        };

        Config cfg{"localhost", 8080};
        Result<Config> result(cfg);

        REQUIRE(result.has_value());
        REQUIRE(result.value().name == "localhost");
        REQUIRE(result.value().port == 8080);
    }
}

TEST_CASE("Error propagation chain", "[error]") {
    SECTION("Function returning Result") {
        auto parseInt = [](const std::string& s) -> Result<int> {
            try {
                return std::stoi(s);
            } catch (const std::exception&) {
                return Error::Validation("Invalid integer", "String: " + s);
            }
        };

        auto addOne = [&](const std::string& s) -> Result<int> {
            auto r = parseInt(s);
            if (!r.has_value()) {
                return r.error();
            }
            return r.value() + 1;
        };

        auto r1 = addOne("42");
        REQUIRE(r1.has_value());
        REQUIRE(r1.value() == 43);

        auto r2 = addOne("invalid");
        REQUIRE_FALSE(r2.has_value());
        REQUIRE(r2.error().message == "Invalid integer");
    }
}

TEST_CASE("Result type alias", "[error]") {
    SECTION("Result<T> is same as Expected<T, Error>") {
        Result<std::string> r1("test");
        REQUIRE(r1.has_value());

        Result<int> r2(Error::Validation("test"));
        REQUIRE_FALSE(r2.has_value());
    }
}
