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
// never printed; the URL goes through PathSchemeUtils::RedactUrl even though
// DuckDB is told to redact as well, because a template can `SET
// redact_http_logs = false`. When off, DuckDB logging is never touched.

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

// Register the sink on `db` and enable DuckDB's HTTP log type with it, with
// `redact_http_logs = true`. Idempotent per database; call it again to put
// back redaction an init statement turned off. Failures are logged, not thrown:
// a debug log must not stop the server.
void EnableDuckDBHttpRequestLog(duckdb_database db);

// ---- request attribution --------------------------------------------------

// While it lives, HTTP requests DuckDB makes on `conn` - on any thread, the
// worker threads of a parallel scan included - are attributed to the flAPI
// request ambient on the constructing thread. A no-op when the switch is off,
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

// "duckdb http GET <redacted url> -> 200 12ms" - no request id, the log
// handler stamps that.
std::string FormatDuckDBHttpLogLine(const DuckDBHttpLogEntry& entry);

}  // namespace flapi
