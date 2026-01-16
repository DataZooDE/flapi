#pragma once

#include <crow.h>
#include <string>
#include <optional>

namespace flapi {

/**
 * Utility functions for common JSON operations.
 * Provides safe string extraction, type checking, and response creation.
 */
class JsonUtils {
public:
    /**
     * Extract string from JSON value.
     * Returns the string value directly, or empty string if value is not a string.
     *
     * @param value The JSON value to extract from
     * @return The extracted string, or empty string if value is not a string
     */
    static std::string extractString(const crow::json::rvalue& value) {
        if (value.t() != crow::json::type::String) {
            return "";
        }

        // For rvalue, .s() gives us the string directly
        return std::string(value.s());
    }

    /**
     * Extract optional string from JSON object by key.
     * Returns nullopt if key is missing or value is not a string.
     *
     * @param json The JSON object to extract from
     * @param key The key to look up
     * @return Optional string value, or nullopt if missing/wrong type
     */
    static std::optional<std::string> extractOptionalString(
        const crow::json::rvalue& json,
        const std::string& key) {

        if (!json.has(key)) {
            return std::nullopt;
        }

        auto value = json[key];
        if (value.t() != crow::json::type::String) {
            return std::nullopt;
        }

        return extractString(value);
    }

    /**
     * Extract required string from JSON object by key.
     * Throws exception if key is missing or value is not a string.
     *
     * @param json The JSON object to extract from
     * @param key The key to look up
     * @param error_msg Optional error message for exception
     * @return The extracted string
     * @throws std::runtime_error if key missing or wrong type
     */
    static std::string extractRequiredString(
        const crow::json::rvalue& json,
        const std::string& key,
        const std::string& error_msg = "") {

        auto result = extractOptionalString(json, key);
        if (!result) {
            std::string msg = error_msg.empty() ? "Missing required field: " + key : error_msg;
            throw std::runtime_error(msg);
        }
        return result.value();
    }

    /**
     * Extract integer from JSON object by key.
     * Returns nullopt if key is missing or value is not an integer.
     *
     * @param json The JSON object to extract from
     * @param key The key to look up
     * @return Optional integer value, or nullopt if missing/wrong type
     */
    static std::optional<int64_t> extractInt(
        const crow::json::rvalue& json,
        const std::string& key) {

        if (!json.has(key)) {
            return std::nullopt;
        }

        auto value = json[key];
        if (value.t() == crow::json::type::Number) {
            return value.i();
        }

        return std::nullopt;
    }

    /**
     * Extract double from JSON object by key.
     * Returns nullopt if key is missing or value is not a number.
     *
     * @param json The JSON object to extract from
     * @param key The key to look up
     * @return Optional double value, or nullopt if missing/wrong type
     */
    static std::optional<double> extractDouble(
        const crow::json::rvalue& json,
        const std::string& key) {

        if (!json.has(key)) {
            return std::nullopt;
        }

        auto value = json[key];
        if (value.t() == crow::json::type::Number) {
            return value.d();
        }

        return std::nullopt;
    }

    /**
     * Extract boolean from JSON object by key.
     * Returns nullopt if key is missing or value is not a boolean.
     *
     * @param json The JSON object to extract from
     * @param key The key to look up
     * @return Optional boolean value, or nullopt if missing/wrong type
     */
    static std::optional<bool> extractBool(
        const crow::json::rvalue& json,
        const std::string& key) {

        if (!json.has(key)) {
            return std::nullopt;
        }

        auto value = json[key];
        if (value.t() == crow::json::type::True) {
            return true;
        } else if (value.t() == crow::json::type::False) {
            return false;
        }

        return std::nullopt;
    }

    /**
     * Convert string to JSON value.
     *
     * @param str The string to convert
     * @return JSON value wrapping the string
     */
    static crow::json::wvalue stringToJson(const std::string& str) {
        crow::json::wvalue result(str);
        return result;
    }

    /**
     * Convert any JSON rvalue to string representation.
     * Handles all types: strings, numbers, booleans, null.
     *
     * @param value The JSON value to convert
     * @return String representation of the value
     */
    static std::string valueToString(const crow::json::rvalue& value) {
        if (value.t() == crow::json::type::String) {
            return extractString(value);
        } else if (value.t() == crow::json::type::Number) {
            // Try integer first, then double
            try {
                return std::to_string(value.i());
            } catch (...) {
                return std::to_string(value.d());
            }
        } else if (value.t() == crow::json::type::True) {
            return "true";
        } else if (value.t() == crow::json::type::False) {
            return "false";
        } else if (value.t() == crow::json::type::Null) {
            return "";
        } else {
            // For objects and arrays, return empty string
            return "";
        }
    }

    /**
     * Convert any JSON wvalue to string representation.
     * Handles all types: strings, numbers, booleans, null.
     * Note: For wvalue, we use dump() for number extraction due to API differences.
     *
     * @param value The JSON value to convert
     * @return String representation of the value
     */
    static std::string valueToString(const crow::json::wvalue& value) {
        if (value.t() == crow::json::type::String) {
            return extractString(value);
        } else if (value.t() == crow::json::type::Number) {
            // For wvalue, use dump() to get the number as string
            return value.dump();
        } else if (value.t() == crow::json::type::True) {
            return "true";
        } else if (value.t() == crow::json::type::False) {
            return "false";
        } else if (value.t() == crow::json::type::Null) {
            return "";
        } else {
            // For objects and arrays, return empty string
            return "";
        }
    }

    /**
     * Create standardized error response.
     *
     * @param code HTTP status code
     * @param message Error message
     * @param details Optional detailed error information
     * @return JSON response with error structure
     */
    static crow::json::wvalue createErrorResponse(
        int code,
        const std::string& message,
        const std::string& details = "") {

        crow::json::wvalue response;
        response["error"]["code"] = code;
        response["error"]["message"] = message;

        if (!details.empty()) {
            response["error"]["details"] = details;
        }

        return response;
    }

    /**
     * Create standardized success response.
     *
     * @param data The data to include in response
     * @return JSON response with success flag and data
     */
    static crow::json::wvalue createSuccessResponse(crow::json::wvalue&& data) {
        crow::json::wvalue response;
        response["success"] = true;
        response["data"] = std::move(data);
        return response;
    }

    /**
     * Merge two JSON objects.
     * Values from source will override values in destination.
     *
     * @param destination Target JSON object
     * @param source Source JSON object
     * @return Reference to destination (for chaining)
     */
    static crow::json::wvalue& mergeJson(
        crow::json::wvalue& destination,
        const crow::json::rvalue& source) {

        // Note: This is a simple merge; for complex scenarios, consider deeper merge
        for (const auto& key : source.keys()) {
            destination[key] = source[key];
        }
        return destination;
    }

    /**
     * Check if JSON value is null.
     *
     * @param value The JSON value to check
     * @return true if value is null, false otherwise
     */
    static bool isNull(const crow::json::rvalue& value) {
        return value.t() == crow::json::type::Null;
    }

    /**
     * Check if JSON value is a string.
     *
     * @param value The JSON value to check
     * @return true if value is a string, false otherwise
     */
    static bool isString(const crow::json::rvalue& value) {
        return value.t() == crow::json::type::String;
    }

    /**
     * Check if JSON value is a number.
     *
     * @param value The JSON value to check
     * @return true if value is a number, false otherwise
     */
    static bool isNumber(const crow::json::rvalue& value) {
        return value.t() == crow::json::type::Number;
    }

    /**
     * Check if JSON value is a boolean.
     *
     * @param value The JSON value to check
     * @return true if value is a boolean, false otherwise
     */
    static bool isBool(const crow::json::rvalue& value) {
        return value.t() == crow::json::type::True || value.t() == crow::json::type::False;
    }

    /**
     * Check if JSON value is an object.
     *
     * @param value The JSON value to check
     * @return true if value is an object, false otherwise
     */
    static bool isObject(const crow::json::rvalue& value) {
        return value.t() == crow::json::type::Object;
    }

    /**
     * Check if JSON value is an array.
     *
     * @param value The JSON value to check
     * @return true if value is an array, false otherwise
     */
    static bool isArray(const crow::json::rvalue& value) {
        return value.t() == crow::json::type::List;
    }

    // ===== Overloads for wvalue types (mutable JSON) =====

    /**
     * Extract string from wvalue.
     * Returns the string value directly by extracting from dump() and removing quotes.
     *
     * @param value The JSON value to extract from
     * @return The extracted string, or empty string if value is not a string
     */
    static std::string extractString(const crow::json::wvalue& value) {
        if (value.t() != crow::json::type::String) {
            return "";
        }

        std::string temp = value.dump();
        if (temp.length() >= 2 && temp.front() == '"' && temp.back() == '"') {
            return temp.substr(1, temp.length() - 2);
        }
        return temp;
    }

    /**
     * Extract optional string from wvalue object by key.
     * Returns nullopt if key is missing or value is not a string.
     *
     * @param json The JSON object to extract from
     * @param key The key to look up
     * @return Optional string value, or nullopt if missing/wrong type
     */
    static std::optional<std::string> extractOptionalString(
        const crow::json::wvalue& json,
        const std::string& key) {

        if (json.count(key) == 0) {
            return std::nullopt;
        }

        auto value = json[key];
        if (value.t() != crow::json::type::String) {
            return std::nullopt;
        }

        return extractString(value);
    }

    /**
     * Check if wvalue is null.
     *
     * @param value The JSON value to check
     * @return true if value is null, false otherwise
     */
    static bool isNull(const crow::json::wvalue& value) {
        return value.t() == crow::json::type::Null;
    }

    /**
     * Check if wvalue is a string.
     *
     * @param value The JSON value to check
     * @return true if value is a string, false otherwise
     */
    static bool isString(const crow::json::wvalue& value) {
        return value.t() == crow::json::type::String;
    }

    /**
     * Check if wvalue is a number.
     *
     * @param value The JSON value to check
     * @return true if value is a number, false otherwise
     */
    static bool isNumber(const crow::json::wvalue& value) {
        return value.t() == crow::json::type::Number;
    }

    /**
     * Check if wvalue is a boolean.
     *
     * @param value The JSON value to check
     * @return true if value is a boolean, false otherwise
     */
    static bool isBool(const crow::json::wvalue& value) {
        return value.t() == crow::json::type::True || value.t() == crow::json::type::False;
    }

    /**
     * Check if wvalue is an object.
     *
     * @param value The JSON value to check
     * @return true if value is an object, false otherwise
     */
    static bool isObject(const crow::json::wvalue& value) {
        return value.t() == crow::json::type::Object;
    }

    /**
     * Check if wvalue is an array.
     *
     * @param value The JSON value to check
     * @return true if value is an array, false otherwise
     */
    static bool isArray(const crow::json::wvalue& value) {
        return value.t() == crow::json::type::List;
    }
};

} // namespace flapi
