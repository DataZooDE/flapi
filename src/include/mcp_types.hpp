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
    std::string id;
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
    std::chrono::steady_clock::time_point created_at;
    std::chrono::steady_clock::time_point last_activity;
    std::unordered_map<std::string, std::string> capabilities;

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
struct MCPStreamingResponse {
    std::string content_type;
    std::string content;
    bool is_complete = false;
    std::unordered_map<std::string, std::string> metadata;
};

// Server capabilities (enhanced)
struct MCPServerCapabilities {
    std::vector<std::string> tools;
    std::vector<std::string> resources;
    std::vector<std::string> prompts;
    std::vector<std::string> sampling;
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
