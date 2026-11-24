#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "duckdb.h"

namespace flapi {

// Opt-in log of every HTTP request DuckDB makes (#252).
//
// Switched on by the top-level `log-http-requests: true` or by the environment
// variable FLAPI_LOG_HTTP_REQUESTS, which wins when set. When on, each DuckDB
// instance flAPI opens (the main database, and the bootstrap instance that reads
// a remote configuration) gets a DuckDBHttpLogSink and DuckDB logging limited to
// the `HTTP` log type. Each entry becomes one info line in the flAPI log:
//
//     duckdb http <METHOD> <url> -> <status|ERR> <N>ms request_id=<id>
//
// <id> is the flAPI request whose query made the request, or "-". Headers are
// never printed. The URL is cut to scheme, host and path with `?redacted` for
// any query (SanitizeLoggedHttpUrl) whatever DuckDB passed, because a template
// can `SET redact_http_logs = false` at any time. When off, DuckDB logging is
// never touched.

// ---- the switch -----------------------------------------------------------

// FLAPI_LOG_HTTP_REQUESTS: 1/true/yes/on or 0/false/no/off (any case); unset or
// anything else is nullopt (the latter with a warning).
std::optional<bool> HttpRequestLogEnvSwitch();

// Settle the switch from the configuration key; the environment variable wins
// when set. Returns the result. Called when the configuration is parsed.
bool ResolveHttpRequestLog(bool config_value);

// The switch: as resolved, or - before the configuration is read - the
// environment variable alone.
bool HttpRequestLogEnabled();

// Forget the resolved value (the environment alone decides again). For tests:
// the switch is process-global.
void ResetHttpRequestLogForTest();

// Register the sink on `db` and enable DuckDB's HTTP log type with it, with
// `redact_http_logs = true`. Idempotent per database; call it again to put
// back redaction an init statement turned off. Failures are logged, not thrown:
// a debug log must not stop the server.
void EnableDuckDBHttpRequestLog(duckdb_database db);

// ---- request attribution --------------------------------------------------

// While it lives, HTTP requests DuckDB makes on `conn` from a thread with no
// request of its own - the worker threads of a parallel scan - are attributed to
// the flAPI request ambient on the constructing thread. A no-op when the switch is off,
// when no request is ambient, or when the connection is already attributed
// (nested execution on the same connection).
class HttpRequestAttributionGuard {
public:
    explicit HttpRequestAttributionGuard(duckdb_connection conn);
    ~HttpRequestAttributionGuard();
    HttpRequestAttributionGuard(const HttpRequestAttributionGuard&) = delete;
    HttpRequestAttributionGuard& operator=(const HttpRequestAttributionGuard&) = delete;

    // The lower-level form, keyed by database instance and DuckDB connection id
    // (what a log entry's context carries). Exposed for tests.
    HttpRequestAttributionGuard(const void* database, std::uint64_t connection_id, std::string request_id);

private:
    const void* database_ = nullptr;
    std::uint64_t connection_id_ = 0;
    bool registered_ = false;
};

// The request id attributed to (database, connection_id), if any.
std::optional<std::string> LookupHttpRequestAttribution(const void* database, std::uint64_t connection_id);

// Whose HTTP request is this: the request ambient on the calling thread first
// (a file handle opened by another connection logs through that connection's
// context, so the map could name the wrong request); then, on a thread with no
// request (a DuckDB worker), the map; then "-".
std::string ResolveHttpRequestId(const void* database, std::optional<std::uint64_t> connection_id);

// ---- formatting (exposed for tests) ---------------------------------------

struct DuckDBHttpLogEntry {
    std::string method;        // GET, HEAD, ...
    std::string url;           // as DuckDB logged it
    std::string status;        // "200", "404", ... or "ERR" when no response
    std::int64_t duration_ms = 0;
};

// Parse an `HTTP` log message. DuckDB 2.0.1 logs the STRUCT of
// HTTPLogType::GetLogType() as its string form; this casts it back to that type
// with DuckDB's own cast, which is what duckdb_logs_parsed() does. nullopt when
// the message does not cast.
std::optional<DuckDBHttpLogEntry> ParseDuckDBHttpLogMessage(const std::string& message);

// The line for one `HTTP` log message: FormatDuckDBHttpLogLine of the parsed
// entry, or "duckdb http ? <unparsed> -> ?" when it does not parse.
std::string DuckDBHttpLogLineFor(const std::string& message);

// "duckdb http GET <url> -> 200 12ms" - no request id, the log handler stamps
// that. The URL goes through SanitizeLoggedHttpUrl.
std::string FormatDuckDBHttpLogLine(const DuckDBHttpLogEntry& entry);

// What DuckDB's BuildLogURL does when it redacts: userinfo (everything before
// the last '@' of the authority) and the fragment dropped, the whole query
// replaced by `?redacted`. Then bytes < 0x21, >= 0x7f and '"' are
// percent-encoded, so the URL is one ASCII token that cannot end the line or
// forge a field. Secrets in the PATH are not touched.
std::string SanitizeLoggedHttpUrl(const std::string& url);

}  // namespace flapi
