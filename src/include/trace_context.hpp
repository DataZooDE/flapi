#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace flapi {

// Bounds on attacker-influenced input. Trace context arrives from an upstream
// flAPI does not control, so every field is capped before it is stored or
// propagated onward.
inline constexpr std::size_t kMaxTracestateBytes = 512;
inline constexpr std::size_t kMaxBaggageBytes = 8192;

// SEP-414 reserves these keys inside MCP's params._meta as UNPREFIXED strings.
//
// They deliberately do NOT live in mcp_constants.hpp next to the
// io.modelcontextprotocol/* keys. SEP-414 makes them an explicit exception to
// MCP's reverse-DNS _meta convention, precisely so implementations do not invent
// io.modelcontextprotocol.traceparent and break correlation with every other
// tracing system. Keeping them physically apart, with this comment, is what stops
// a future maintainer "tidying" them into the namespaced block.
namespace sep414 {
inline constexpr const char* kTraceparent = "traceparent";
inline constexpr const char* kTracestate = "tracestate";
inline constexpr const char* kBaggage = "baggage";
}  // namespace sep414

struct SpanContextIds {
    std::string trace_id;    // 32 lowercase hex, never all-zero
    std::string span_id;     // 16 lowercase hex, never all-zero
    std::uint8_t flags = 0;
    std::string tracestate;  // opaque, clamped, propagated untouched
    std::string baggage;     // extracted and propagated, NEVER a span attribute

    bool valid() const { return trace_id.size() == 32 && span_id.size() == 16; }
    bool sampled() const { return (flags & 0x01) != 0; }
};

enum class ContextSource {
    None,             // no usable context; start a new root trace
    Header,           // W3C traceparent HTTP header
    Meta,             // MCP params._meta (SEP-414)
    MetaOverHeader,   // both present and DISAGREEING; _meta won
};

struct ExtractedContext {
    SpanContextIds ids;
    ContextSource source = ContextSource::None;
};

// Strict W3C parse. Returns an invalid SpanContextIds for anything malformed and
// NEVER throws: a broken header from an upstream must start a new root trace, not
// fail the request.
SpanContextIds parseTraceparent(std::string_view traceparent,
                                std::string_view tracestate = {},
                                std::string_view baggage = {});

std::string formatTraceparent(const SpanContextIds& ids);

// Precedence, per the MCP semantic conventions: params._meta beats the HTTP
// header. Over a proxy or gateway the HTTP hop may be the gateway's own span
// while _meta carries the agent's, so preferring _meta keeps flAPI attached to
// the trace a user actually cares about. Disagreement is reported rather than
// hidden, so the situation is diagnosable.
ExtractedContext resolvePrecedence(const SpanContextIds& from_meta,
                                   const SpanContextIds& from_header);

const char* contextSourceName(ContextSource source);

}  // namespace flapi
