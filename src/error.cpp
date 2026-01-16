#include "error.hpp"

namespace flapi {

std::string Error::getCategoryName() const {
    switch (category) {
        case ErrorCategory::Configuration:
            return "Configuration";
        case ErrorCategory::Database:
            return "Database";
        case ErrorCategory::Validation:
            return "Validation";
        case ErrorCategory::Authentication:
            return "Authentication";
        case ErrorCategory::NotFound:
            return "NotFound";
        case ErrorCategory::Internal:
            return "Internal";
        default:
            return "Unknown";
    }
}

crow::response Error::toHttpResponse() const {
    crow::json::wvalue error_json;
    error_json["success"] = false;
    error_json["error"]["category"] = getCategoryName();
    error_json["error"]["message"] = message;

    if (!details.empty()) {
        error_json["error"]["details"] = details;
    }

    return crow::response(http_status_code, error_json);
}

crow::json::wvalue Error::toJson() const {
    crow::json::wvalue error_json;
    error_json["success"] = false;
    error_json["error"]["category"] = getCategoryName();
    error_json["error"]["message"] = message;

    if (!details.empty()) {
        error_json["error"]["details"] = details;
    }

    return error_json;
}

} // namespace flapi
