#pragma once

#include <crow.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <optional>

namespace flapi {

// Core JSON-RPC types
struct MCPRequest {
    std::string jsonrpc = "2.0";
    // Human-readable id (unquoted string, number text, or "" for null/absent).
    // Kept for logging and for MCPResponse.id round-tripping.
    std::string id;
    // JSON-RPC id handling. `id_present == false` means the request carried no
    // `id` member at all — i.e. it is a notification and MUST NOT be answered.
    // `id_raw` is the verbatim JSON token of the id ("42", "\"abc\"", "null"),
    // echoed back losslessly (crow preserves int64/uint64) instead of being
    // re-parsed through std::stod, which mangled large integer ids.
    bool id_present = false;
    std::string id_raw;
    std::string method;
    crow::json::wvalue params;
};

struct MCPResponse {
    std::string jsonrpc = "2.0";
    std::string id;
    std::string result;
    std::string error;
};

// Session management
struct MCPSession {
    std::string session_id;
    std::string client_version;
    std::string protocol_version = "2025-11-25";  // Negotiated protocol version
    std::chrono::steady_clock::time_point created_at;
    std::chrono::steady_clock::time_point last_activity;
    std::unordered_map<std::string, std::string> capabilities;

    // Authentication context (optional)
    struct AuthContext {
        bool authenticated = false;
        std::string username;
        std::vector<std::string> roles;
        std::chrono::steady_clock::time_point auth_time;
        std::string auth_type;  // "basic", "bearer", or "oidc"

        // OIDC-specific fields
        std::string bound_token_jti;  // Bind token ID to session (prevent hijacking)
        std::chrono::steady_clock::time_point token_expires_at;  // Token expiration time
        std::string refresh_token;    // For token refresh (optional)

        // Check if OIDC token needs refresh (within 5 minutes of expiration)
        bool needsTokenRefresh() const {
            if (auth_type != "oidc") {
                return false;
            }
            auto now = std::chrono::steady_clock::now();
            auto refresh_threshold = token_expires_at - std::chrono::minutes(5);
            return now >= refresh_threshold;
        }

        // Check if OIDC token is expired
        bool isTokenExpired() const {
            if (auth_type != "oidc") {
                return false;
            }
            return std::chrono::steady_clock::now() >= token_expires_at;
        }
    };
    std::optional<AuthContext> auth_context;

    // Check if session is authenticated
    bool isAuthenticated() const {
        return auth_context && auth_context->authenticated;
    }

    MCPSession() : created_at(std::chrono::steady_clock::now()),
                   last_activity(created_at) {}
};

struct MCPClientCapabilities {
    bool supports_sampling = false;
    bool supports_roots = false;
    bool supports_logging = false;
    std::vector<std::string> supported_protocols;

    MCPClientCapabilities() = default;
};

// Streaming response for SSE support
// Server capabilities (enhanced)
struct MCPServerCapabilities {
    std::vector<std::string> tools;
    std::vector<std::string> resources;
    std::vector<std::string> prompts;
    std::vector<std::string> logging;

    MCPServerCapabilities() = default;
};

// Server info (enhanced)
struct MCPServerInfo {
    std::string name;
    std::string version;
    std::string protocol_version;

    MCPServerInfo() = default;
};

// Prompt types
struct MCPPromptArgument {
    std::string name;
    std::string description;
    std::string type = "string"; // string, number, boolean, array
    bool required = false;
    crow::json::wvalue default_value = nullptr;
};

struct MCPPromptInfo {
    std::string name;
    std::string description;
    std::string template_content;
    std::vector<MCPPromptArgument> arguments;
};

// JSON-RPC error response
struct MCPError {
    int code;
    std::string message;
    crow::json::wvalue data;

    MCPError(int error_code, const std::string& error_message)
        : code(error_code), message(error_message) {}
};

} // namespace flapi
