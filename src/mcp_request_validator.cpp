#include "include/mcp_request_validator.hpp"
#include "include/mcp_constants.hpp"
#include <algorithm>
#include <regex>

namespace flapi {

std::vector<std::string> MCPRequestValidator::validation_errors_;

bool MCPRequestValidator::validateJsonRpcRequest(const MCPRequest& request) {
    clearValidationErrors();

    // Validate JSON-RPC version
    if (!isValidJsonRpcVersion(request.jsonrpc)) {
        validation_errors_.push_back("Invalid JSON-RPC version: " + request.jsonrpc);
        return false;
    }

    // Validate method name
    if (!isValidMethodName(request.method)) {
        validation_errors_.push_back("Invalid method name: " + request.method);
        return false;
    }

    // Method-specific validation
    if (!validateParamsForMethod(request.method, request.params)) {
        return false;  // validateParamsForMethod already adds errors
    }

    return true;
}

bool MCPRequestValidator::validateMethodExists(const std::string& method) {
    clearValidationErrors();

    std::vector<std::string> valid_methods = {
        flapi::mcp::constants::METHOD_INITIALIZE,
        flapi::mcp::constants::METHOD_TOOLS_LIST,
        flapi::mcp::constants::METHOD_TOOLS_CALL,
        flapi::mcp::constants::METHOD_RESOURCES_LIST,
        flapi::mcp::constants::METHOD_RESOURCES_READ
    };

    auto it = std::find(valid_methods.begin(), valid_methods.end(), method);
    if (it == valid_methods.end()) {
        validation_errors_.push_back("Method not found: " + method);
        return false;
    }

    return true;
}

bool MCPRequestValidator::validateParamsForMethod(const std::string& method,
                                                const crow::json::wvalue& params) {
    if (method == flapi::mcp::constants::METHOD_INITIALIZE) {
        return validateInitializeParams(params);
    } else if (method == flapi::mcp::constants::METHOD_TOOLS_CALL) {
        return validateToolsCallParams(params);
    } else if (method == flapi::mcp::constants::METHOD_RESOURCES_READ) {
        return validateResourcesReadParams(params);
    }

    // Other methods don't require specific param validation for now
    return true;
}

bool MCPRequestValidator::validateAcceptHeader(const std::string& accept_header) {
    if (accept_header.empty()) {
        return false;
    }

    // Check for required content types
    bool has_json = accept_header.find("application/json") != std::string::npos;
    bool has_event_stream = accept_header.find("text/event-stream") != std::string::npos;

    return has_json && has_event_stream;
}

bool MCPRequestValidator::validateContentType(const std::string& content_type) {
    return content_type == flapi::mcp::constants::CONTENT_TYPE_JSON;
}

std::vector<std::string> MCPRequestValidator::getValidationErrors() {
    return validation_errors_;
}

void MCPRequestValidator::clearValidationErrors() {
    validation_errors_.clear();
}

bool MCPRequestValidator::isValidJsonRpcVersion(const std::string& version) {
    return version == flapi::mcp::constants::JSONRPC_VERSION;
}

bool MCPRequestValidator::isValidMethodName(const std::string& method) {
    // Method names should not be empty and should match pattern
    if (method.empty()) {
        return false;
    }

    // Basic validation - method should contain only allowed characters
    std::regex method_pattern(R"(^[a-zA-Z_][a-zA-Z0-9_./]*$)");
    return std::regex_match(method, method_pattern);
}

bool MCPRequestValidator::validateInitializeParams(const crow::json::wvalue& params) {
    // Initialize params should be an object
    if (params.t() != crow::json::type::Object) {
        validation_errors_.emplace_back("Initialize params must be an object");
        return false;
    }

    // Optional: Check for protocol version
    if (params.count("protocolVersion") > 0) {
        auto version = params["protocolVersion"];
        if (version.t() == crow::json::type::String) {
            std::string version_str = version.dump();
            // Accept common protocol versions
            if (version_str != "2024-11-05" && version_str != "2024-01-01") {
                validation_errors_.push_back("Unsupported protocol version: " + version_str);
                return false;
            }
        }
    }

    return true;
}

bool MCPRequestValidator::validateToolsCallParams(const crow::json::wvalue& params) {
    // Tools call params should be an object
    if (params.t() != crow::json::type::Object) {
        validation_errors_.emplace_back("Tools call params must be an object");
        return false;
    }

    // Must have 'name' field
    if (params.count("name") == 0) {
        validation_errors_.emplace_back("Tools call params must include 'name' field");
        return false;
    }

    auto name_value = params["name"];
    if (name_value.t() != crow::json::type::String) {
        validation_errors_.emplace_back("Tool name must be a string");
        return false;
    }

    return true;
}

bool MCPRequestValidator::validateResourcesReadParams(const crow::json::wvalue& params) {
    // Resources read params should be an object
    if (params.t() != crow::json::type::Object) {
        validation_errors_.emplace_back("Resources read params must be an object");
        return false;
    }

    // Must have 'uri' field
    if (params.count("uri") == 0) {
        validation_errors_.emplace_back("Resources read params must include 'uri' field");
        return false;
    }

    auto uri_value = params["uri"];
    if (uri_value.t() != crow::json::type::String) {
        validation_errors_.emplace_back("Resource URI must be a string");
        return false;
    }

    return true;
}

} // namespace flapi
