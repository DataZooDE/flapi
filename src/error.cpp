#include "error.hpp"
#include "datazoo_banner.hpp"

namespace {
// Repo identity for the issue link. Duplicated rather than shared with main.cpp
// so error.cpp stays independent of the entry point's translation unit.
constexpr datazoo::BannerInfo kFlapiBanner {"flapi", "", "https://github.com/DataZooDE/flapi"};
} // namespace

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

    // A structured field rather than text appended to `message`: this payload is
    // parsed by clients, so the link has to be additive and ignorable. Anything
    // concatenated into the message would change what existing consumers read.
    error_json["error"]["report_issue"] = datazoo::IssuesUrl(kFlapiBanner);

    return error_json;
}

} // namespace flapi
