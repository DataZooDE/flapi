#pragma once

#include "mcp_types.hpp"
#include <crow.h>
#include <string>
#include <vector>

namespace flapi {

class MCPRequestValidator {
public:
    // Validate JSON-RPC request structure
    static bool validateJsonRpcRequest(const MCPRequest& request);

    // Validate specific methods
    static bool validateMethodExists(const std::string& method);
    static bool validateParamsForMethod(const std::string& method,
                                      const crow::json::wvalue& params);

    // HTTP validation
    static bool validateAcceptHeader(const std::string& accept_header);
    static bool validateContentType(const std::string& content_type);

    // Get validation errors
    static std::vector<std::string> getValidationErrors();
    static void clearValidationErrors();

    // Utility methods
    static bool isValidJsonRpcVersion(const std::string& version);
    static bool isValidMethodName(const std::string& method);

private:
    static std::vector<std::string> validation_errors_;

    // Method-specific validation
    static bool validateInitializeParams(const crow::json::wvalue& params);
    static bool validateToolsCallParams(const crow::json::wvalue& params);
    static bool validateResourcesReadParams(const crow::json::wvalue& params);
};

} // namespace flapi
