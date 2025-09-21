#pragma once

#include "mcp_types.hpp"
#include <crow.h>
#include <string>

namespace flapi {

class MCPErrorBuilder {
public:
    // Create JSON-RPC error responses
    static crow::response createJsonRpcError(const std::string& id,
                                           int code,
                                           const std::string& message,
                                           const crow::json::wvalue& data = nullptr);

    // Create HTTP error responses
    static crow::response createHttpError(int http_code,
                                        const std::string& message);

    // Create JSON-RPC error JSON
    static std::string createJsonRpcErrorJson(int code,
                                            const std::string& message,
                                            const std::string& id = "");

    // Create enhanced error with context
    static std::string createEnhancedJsonRpcError(int code,
                                                const std::string& message,
                                                const std::string& id,
                                                const std::string& context);

private:
    // Helper to format JSON error response
    static std::string formatErrorResponse(int code,
                                         const std::string& message,
                                         const std::string& id);
};

} // namespace flapi
