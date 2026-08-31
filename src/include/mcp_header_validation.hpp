#pragma once

#include <string>

namespace flapi::mcp {

// MCP 2026-07-28 mirrored-header validation (Streamable HTTP). On the modern
// path a client MUST mirror the request into HTTP headers so an edge proxy can
// route and rate-limit without parsing the JSON-RPC body:
//   MCP-Protocol-Version : the _meta protocolVersion
//   Mcp-Method           : the JSON-RPC method
//   Mcp-Name             : params.name (tools/call, prompts/get) or params.uri
//                          (resources/read)
// Non-ASCII header values may be base64-sentinel encoded as `=?base64?<b64>?=`.
// A mismatch between a header and the body is a -32020 HeaderMismatch error.
//
// These are pure functions with no Crow/JSON coupling so they can be exercised
// directly by the spec's value-encoding table.

// Decode a possibly sentinel-encoded header value. If `value` is exactly
// `=?base64?<b64>?=` (markers lowercase), returns the base64-decoded payload;
// otherwise returns `value` unchanged. On malformed base64 inside a sentinel,
// returns the original value (the comparison will then fail, yielding -32020).
std::string decodeSentinel(const std::string& value);

// True if the two strings are equal, or if both parse as the same JSON number
// (so "42" matches "42.0" and " 42 " matches "42"). Used because a mirrored
// integer parameter may be rendered differently in the header and the body.
bool numericEquals(const std::string& a, const std::string& b);

// Compare a decoded header value against the expected body value, using numeric
// equality as a fallback. Returns true on match.
bool headerMatches(const std::string& header_value, const std::string& body_value);

} // namespace flapi::mcp
