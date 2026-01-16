#pragma once

#include <string>
#include <variant>
#include <crow.h>

namespace flapi {

// Error categories for classification and HTTP status mapping
enum class ErrorCategory {
    Configuration,    // Config file/structure issues
    Database,        // Database/query execution errors
    Validation,      // Input validation failures
    Authentication,  // Authentication/authorization failures
    NotFound,        // Resource not found
    Internal         // Internal/programming errors
};

// Error details structure
struct Error {
    ErrorCategory category;
    std::string message;
    std::string details;
    int http_status_code;

    // Factory methods for common error types
    static Error Config(const std::string& msg, const std::string& details = "") {
        return Error{ErrorCategory::Configuration, msg, details, 500};
    }

    static Error Database(const std::string& msg, const std::string& details = "") {
        return Error{ErrorCategory::Database, msg, details, 500};
    }

    static Error Validation(const std::string& msg, const std::string& details = "") {
        return Error{ErrorCategory::Validation, msg, details, 400};
    }

    static Error Auth(const std::string& msg, const std::string& details = "") {
        return Error{ErrorCategory::Authentication, msg, details, 401};
    }

    static Error NotFound(const std::string& msg, const std::string& details = "") {
        return Error{ErrorCategory::NotFound, msg, details, 404};
    }

    static Error Internal(const std::string& msg, const std::string& details = "") {
        return Error{ErrorCategory::Internal, msg, details, 500};
    }

    // Convert error to HTTP response
    crow::response toHttpResponse() const;

    // Convert error to JSON representation
    crow::json::wvalue toJson() const;

    // Get category name as string
    std::string getCategoryName() const;
};

// Expected<T, E> is a sum type that can hold either a success value or an error
// This is the Result type pattern for operations that can fail
template<typename T, typename E = Error>
class Expected {
public:
    // Constructor for value types (T && lvalue ref, excluding E type and Expected itself)
    template<typename U,
             typename std::enable_if_t<!std::is_same_v<std::decay_t<U>, E> &&
                                       !std::is_same_v<std::decay_t<U>, Expected>,
                                       int> = 0>
    Expected(U&& val) : has_value_(true) {
        new (&value_) T(std::forward<U>(val));
    }

    // Constructor for error types (E && references)
    // Only enabled when U decays to E
    template<typename U,
             typename std::enable_if_t<std::is_same_v<std::decay_t<U>, E>,
                                       int> = 0>
    Expected(U&& err) : has_value_(false) {
        new (&error_) E(std::forward<U>(err));
    }

    // Copy constructor deleted
    Expected(const Expected&) = delete;
    Expected& operator=(const Expected&) = delete;

    // Move constructor
    Expected(Expected&& other) noexcept : has_value_(other.has_value_) {
        if (has_value_) {
            new (&value_) T(std::move(other.value_));
        } else {
            new (&error_) E(std::move(other.error_));
        }
    }

    // Move assignment
    Expected& operator=(Expected&& other) noexcept {
        if (this != &other) {
            this->~Expected();
            has_value_ = other.has_value_;
            if (has_value_) {
                new (&value_) T(std::move(other.value_));
            } else {
                new (&error_) E(std::move(other.error_));
            }
        }
        return *this;
    }

    // Destructor
    ~Expected() {
        if (has_value_) {
            value_.~T();
        } else {
            error_.~E();
        }
    }

    // Check if contains value (success)
    bool has_value() const { return has_value_; }
    explicit operator bool() const { return has_value_; }

    // Get the value (only valid if has_value() is true)
    T& value() {
        if (!has_value_) throw std::runtime_error("Accessing value of error Expected");
        return value_;
    }

    const T& value() const {
        if (!has_value_) throw std::runtime_error("Accessing value of error Expected");
        return value_;
    }

    // Get the error (only valid if has_value() is false)
    E& error() {
        if (has_value_) throw std::runtime_error("Accessing error of success Expected");
        return error_;
    }

    const E& error() const {
        if (has_value_) throw std::runtime_error("Accessing error of success Expected");
        return error_;
    }

    // Dereference operators for convenience
    T& operator*() { return value(); }
    const T& operator*() const { return value(); }

    T* operator->() { return &value(); }
    const T* operator->() const { return &value(); }

private:
    union {
        T value_;
        E error_;
    };
    bool has_value_;
};

// Alias for common Result type: Result<T> means Result<T, Error>
template<typename T>
using Result = Expected<T, Error>;

} // namespace flapi
