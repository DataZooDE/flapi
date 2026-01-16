#pragma once

#include <string>
#include <chrono>

namespace flapi::mcp::constants {

// JSON-RPC 2.0 Specification Constants
constexpr const char* JSONRPC_VERSION = "2.0";
constexpr const char* MCP_SESSION_HEADER = "Mcp-Session-Id";
constexpr int DEFAULT_SESSION_TIMEOUT_MINUTES = 30;

// MCP Protocol Version Constants
constexpr const char* MCP_LATEST_PROTOCOL_VERSION = "2025-11-25";
constexpr const char* MCP_PROTOCOL_VERSION_2025_11_25 = "2025-11-25";
constexpr const char* MCP_PROTOCOL_VERSION_2025_06_18 = "2025-06-18";
constexpr const char* MCP_PROTOCOL_VERSION_2025_03_26 = "2025-03-26";
constexpr const char* MCP_PROTOCOL_VERSION_2024_11_05 = "2024-11-05";

// HTTP Status Codes
constexpr int HTTP_OK = 200;
constexpr int HTTP_BAD_REQUEST = 400;
constexpr int HTTP_NOT_ACCEPTABLE = 406;
constexpr int HTTP_INTERNAL_SERVER_ERROR = 500;

// JSON-RPC Error Codes
constexpr int PARSE_ERROR = -32700;
constexpr int INVALID_REQUEST = -32600;
constexpr int METHOD_NOT_FOUND = -32601;
constexpr int INVALID_PARAMS = -32602;
constexpr int INTERNAL_ERROR = -32603;

// Content Types
constexpr const char* CONTENT_TYPE_JSON = "application/json";
constexpr const char* CONTENT_TYPE_SSE = "text/event-stream";

// MCP Protocol Methods
constexpr const char* METHOD_INITIALIZE = "initialize";
constexpr const char* METHOD_TOOLS_LIST = "tools/list";
constexpr const char* METHOD_TOOLS_CALL = "tools/call";
constexpr const char* METHOD_RESOURCES_LIST = "resources/list";
constexpr const char* METHOD_RESOURCES_READ = "resources/read";
constexpr const char* METHOD_RESOURCES_TEMPLATES_LIST = "resources/templates/list";
constexpr const char* METHOD_PROMPTS_LIST = "prompts/list";
constexpr const char* METHOD_PROMPTS_GET = "prompts/get";
constexpr const char* METHOD_PING = "ping";
constexpr const char* METHOD_LOGGING_SET_LEVEL = "logging/setLevel";
constexpr const char* METHOD_COMPLETION_COMPLETE = "completion/complete";

// Server Capabilities
constexpr const char* CAPABILITY_TOOLS = "tools";
constexpr const char* CAPABILITY_RESOURCES = "resources";
constexpr const char* CAPABILITY_PROMPTS = "prompts";
constexpr const char* CAPABILITY_SAMPLING = "sampling";

} // namespace flapi::mcp::constants
