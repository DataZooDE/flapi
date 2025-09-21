#include "include/mcp_error_builder.hpp"
#include "include/mcp_constants.hpp"
#include <crow.h>
#include <sstream>

namespace flapi {

crow::response MCPErrorBuilder::createJsonRpcError(const std::string& id,
                                                 int code,
                                                 const std::string& message,
                                                 const crow::json::wvalue& data) {
    crow::json::wvalue error_json;
    error_json["code"] = code;
    error_json["message"] = message;

    if (data.t() != crow::json::type::Null) {
        error_json["data"] = crow::json::wvalue(data);
    }

    crow::json::wvalue response_json;
    response_json["jsonrpc"] = flapi::mcp::constants::JSONRPC_VERSION;

    // Handle id field properly - can be string, number, or null
    if (id.empty()) {
        response_json["id"] = nullptr;  // JSON null for empty/null id
    } else {
        // Try to parse as number first, fallback to string
        try {
            if (id.find_first_not_of("0123456789.-") == std::string::npos) {
                response_json["id"] = std::stod(id);
            } else {
                response_json["id"] = id;
            }
        } catch (const std::exception&) {
            response_json["id"] = id;
        }
    }

    response_json["error"] = std::move(error_json);

    auto response = crow::response(flapi::mcp::constants::HTTP_BAD_REQUEST, response_json.dump());
    response.set_header("Content-Type", flapi::mcp::constants::CONTENT_TYPE_JSON);
    return response;
}

crow::response MCPErrorBuilder::createHttpError(int http_code, const std::string& message) {
    crow::json::wvalue error_json;
    error_json["error"] = message;

    auto response = crow::response(http_code, error_json.dump());
    response.set_header("Content-Type", flapi::mcp::constants::CONTENT_TYPE_JSON);
    return response;
}

std::string MCPErrorBuilder::createJsonRpcErrorJson(int code,
                                                  const std::string& message,
                                                  const std::string& id) {
    return formatErrorResponse(code, message, id);
}

std::string MCPErrorBuilder::createEnhancedJsonRpcError(int code,
                                                       const std::string& message,
                                                       const std::string& id,
                                                       const std::string& context) {
    std::stringstream ss;
    ss << message;

    if (!context.empty()) {
        ss << " (Context: " << context << ")";
    }

    return formatErrorResponse(code, ss.str(), id);
}

std::string MCPErrorBuilder::formatErrorResponse(int code,
                                               const std::string& message,
                                               const std::string& id) {
    std::stringstream ss;
    ss << R"({"jsonrpc":"2.0","id":)";

    if (id.empty()) {
        ss << "null";
    } else if (id.find_first_not_of("0123456789.-") == std::string::npos) {
        ss << id;  // It's a number
    } else {
        ss << "\"" << id << "\"";  // It's a string
    }

    ss << R"(,"error":{"code":)" << code << R"(,"message":")" << message << R"("}})";

    return ss.str();
}

} // namespace flapi
