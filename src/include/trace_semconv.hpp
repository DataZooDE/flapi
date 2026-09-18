#pragma once

namespace flapi::semconv {

// Every attribute key in one place.
//
// The GenAI conventions are Development-stage and have already changed
// repositories once; HTTP is Stable. Keeping the keys here as data means a
// convention rename is a one-file change rather than a hunt through call sites,
// which is exactly what the risk register asked for.
//
// Pinned revision: OpenTelemetry semantic conventions v1.38.0 (HTTP: Stable;
// gen_ai.* / mcp.*: Development). Update this comment with the keys.

namespace http {
inline constexpr auto kRequestMethod   = "http.request.method";
inline constexpr auto kRoute           = "http.route";
inline constexpr auto kResponseStatus  = "http.response.status_code";
inline constexpr auto kRequestBodySize = "http.request.body.size";
inline constexpr auto kResponseBodySize= "http.response.body.size";
}  // namespace http

namespace url {
inline constexpr auto kScheme = "url.scheme";
inline constexpr auto kPath   = "url.path";       // TEMPLATE only, never filled
}  // namespace url

namespace net {
inline constexpr auto kServerAddress   = "server.address";
inline constexpr auto kServerPort      = "server.port";
inline constexpr auto kClientAddress   = "client.address";
inline constexpr auto kProtocolName    = "network.protocol.name";
inline constexpr auto kProtocolVersion = "network.protocol.version";
inline constexpr auto kTransport       = "network.transport";
}  // namespace net

inline constexpr auto kErrorType       = "error.type";
inline constexpr auto kUserAgent       = "user_agent.original";

namespace mcp {
inline constexpr auto kMethodName      = "mcp.method.name";
inline constexpr auto kSessionId       = "mcp.session.id";
inline constexpr auto kProtocolVersion = "mcp.protocol.version";
inline constexpr auto kRpcRequestId    = "jsonrpc.request.id";
inline constexpr auto kRpcStatusCode   = "rpc.response.status_code";
}  // namespace mcp

namespace genai {
inline constexpr auto kOperationName = "gen_ai.operation.name";
inline constexpr auto kToolName      = "gen_ai.tool.name";
inline constexpr auto kExecuteTool   = "execute_tool";
}  // namespace genai


// DuckDB execution profiling (tracing.db_profiling). flapi.* rather than db.*:
// these are DuckDB's own metrics, not stable OpenTelemetry semantic conventions,
// and must not be mistaken for them.
//
// This list is an ALLOWLIST and the implementation reads only these keys. It
// must never iterate DuckDB's metric map: QUERY_NAME is the SQL text and
// EXTRA_INFO is the rendered filter predicate, which on the prepared path
// carries bound parameter values.
namespace dbprof {
constexpr const char* kLatencyMs        = "flapi.db.latency_ms";
constexpr const char* kBlockedMs        = "flapi.db.blocked_thread_time_ms";
constexpr const char* kResultBytes      = "flapi.db.result_set_bytes";
constexpr const char* kBytesRead        = "flapi.db.bytes_read";
constexpr const char* kCpuTimeMs        = "flapi.db.cpu_time_ms";        // detailed only
constexpr const char* kRowsScanned      = "flapi.db.rows_scanned";       // detailed only
}  // namespace dbprof

namespace db {
inline constexpr auto kSystemName     = "db.system.name";
inline constexpr auto kOperationName  = "db.operation.name";
inline constexpr auto kNamespace      = "db.namespace";
inline constexpr auto kReturnedRows   = "db.response.returned_rows";
inline constexpr auto kQueryText      = "db.query.text";
inline constexpr auto kDuckDB         = "duckdb";
}  // namespace db

// flAPI's own attributes. Not semconv, so they carry the flapi. prefix and are
// free to describe things no convention covers.
namespace flapix {
inline constexpr auto kContextSource   = "flapi.trace.context_source";
inline constexpr auto kResponseFormat  = "flapi.response.format";
inline constexpr auto kAuthKind        = "flapi.auth.kind";
inline constexpr auto kAuthzOutcome    = "flapi.authz.outcome";
inline constexpr auto kRateLimitApplied= "flapi.rate_limit.applied";
inline constexpr auto kCorsPreflight   = "flapi.cors.preflight";
inline constexpr auto kTemplatePath    = "flapi.template.path";
inline constexpr auto kTemplateBytes   = "flapi.template.bytes";
inline constexpr auto kParamsCount     = "flapi.params.count";
inline constexpr auto kPreparedCount   = "flapi.params.prepared_count";
inline constexpr auto kInterpolatedCount = "flapi.params.interpolated_count";
inline constexpr auto kResultBytes     = "flapi.result.bytes";
inline constexpr auto kSerializeFormat = "flapi.serialize.format";
// Deliberately "backed", not "hit": TELEMETRY.md already documents that flAPI
// has no per-request cache hit/miss signal, and a span must not imply one.
inline constexpr auto kCacheBacked     = "flapi.cache.backed";
inline constexpr auto kHasQuery        = "flapi.url.has_query";
}  // namespace flapix

// Every unmatched path collapses here. Attacker-controlled URLs must never mint
// distinct span names or metric series: that is both a cardinality blow-up and a
// cost-amplification vector against whoever pays for ingest.
inline constexpr auto kUnmatchedRoute = "<unmatched>";

}  // namespace flapi::semconv
