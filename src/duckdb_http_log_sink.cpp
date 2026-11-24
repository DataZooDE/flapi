#include "duckdb_http_log_sink.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string_view>
#include <utility>

#include <crow/logging.h>

#include "duckdb.hpp"
#include "duckdb/logging/log_manager.hpp"
#include "duckdb/logging/log_sink.hpp"
#include "duckdb/logging/log_type.hpp"
#include "duckdb/main/capi/capi_internal.hpp"

#include "flapi_log_handler.hpp"
#include "request_context.hpp"
#include "vfs_adapter.hpp"

namespace flapi {

namespace {

constexpr const char* kSinkName = "flapi_http";
constexpr const char* kUnparsedLine = "duckdb http ? <unparsed> -> ?";

// ---- the switch -----------------------------------------------------------

// -1: not resolved from a configuration yet (the environment alone decides).
std::atomic<int> g_resolved{-1};

std::string lowered(std::string_view in) {
    std::string out(in);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// ---- request attribution --------------------------------------------------

using AttributionKey = std::pair<const void*, std::uint64_t>;

std::mutex g_attribution_mutex;
std::map<AttributionKey, std::string> g_attribution;

// ---- the sink -------------------------------------------------------------

std::string statusOf(const duckdb::Value& response) {
    // No response at all: the request failed before one arrived.
    if (response.IsNull()) {
        return "ERR";
    }
    const auto& type = response.type();
    const auto& children = duckdb::StructValue::GetChildren(response);
    for (duckdb::idx_t i = 0; i < children.size(); ++i) {
        if (duckdb::StructType::GetChildName(type, i).GetIdentifierName() != "status" || children[i].IsNull()) {
            continue;
        }
        // HTTPStatusToLogString: the enum name ("OK_200", "NotFound_404"), or the
        // bare number for a code the enum does not name. INVALID (0) means the
        // client got no response.
        const std::string status = children[i].ToString();
        const auto underscore = status.rfind('_');
        const std::string digits = underscore == std::string::npos ? status : status.substr(underscore + 1);
        if (digits.size() == 3 && std::all_of(digits.begin(), digits.end(), [](unsigned char c) {
                return std::isdigit(c) != 0;
            }) && digits[0] >= '1' && digits[0] <= '5') {
            return digits;
        }
    }
    return "ERR";
}

const duckdb::Value* childNamed(const duckdb::Value& parent, std::string_view name) {
    if (parent.IsNull() || parent.type().id() != duckdb::LogicalTypeId::STRUCT) {
        return nullptr;
    }
    const auto& children = duckdb::StructValue::GetChildren(parent);
    for (duckdb::idx_t i = 0; i < children.size(); ++i) {
        if (duckdb::StructType::GetChildName(parent.type(), i).GetIdentifierName() == name) {
            return &children[i];
        }
    }
    return nullptr;
}

// Never more than one warning a minute from the sink: it runs on DuckDB's HTTP
// path, possibly for every request.
void warnRateLimited(const char* what) noexcept {
    static std::atomic<std::int64_t> last_ms{0};
    try {
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
        auto previous = last_ms.load(std::memory_order_relaxed);
        if (now - previous < 60'000 && previous != 0) {
            return;
        }
        if (!last_ms.compare_exchange_strong(previous, now)) {
            return;
        }
        CROW_LOG_WARNING << "duckdb http: " << what;
    } catch (...) {
    }
}

bool infoLogged() {
    return crow::logger::get_current_log_level() <= crow::LogLevel::Info;
}

class DuckDBHttpLogSink : public duckdb::LogSink {
public:
    explicit DuckDBHttpLogSink(const void* database) : database_(database) {}

    const std::string GetSinkName() override { return kSinkName; }

    // Called by the LogManager under its own lock, synchronously, on the thread
    // that made the request. Nothing is buffered: the line is in the flAPI log
    // when DuckDB returns from the request.
    //
    // Nothing may escape into DuckDB: HTTPUtil::LogRequest has no handler, and it
    // also runs while an exception is already unwinding.
    void WriteLogEntry(duckdb::timestamp_t, duckdb::LogLevel, const std::string& log_type,
                       const std::string& log_message, const duckdb::RegisteredLoggingContext& context) override {
        try {
            // All of this runs under the LogManager's lock: skip the work when the
            // line would be filtered anyway.
            if (!infoLogged() || log_type != duckdb::HTTPLogType::NAME) {
                return;
            }
            write(log_message, context.context.connection_id);
        } catch (...) {
            warnRateLimited("a log entry could not be written");
        }
    }

    void WriteLogEntries(duckdb::DataChunk& chunk, const duckdb::RegisteredLoggingContext& context) override {
        try {
            if (!infoLogged()) {
                return;
            }
            // LOG_ENTRIES layout: context_id, timestamp, type, log_level, message.
            for (duckdb::idx_t row = 0; row < chunk.size(); ++row) {
                const auto type = chunk.GetValue(2, row);
                const auto message = chunk.GetValue(4, row);
                if (type.IsNull() || message.IsNull() || type.ToString() != duckdb::HTTPLogType::NAME) {
                    continue;
                }
                write(message.ToString(), context.context.connection_id);
            }
        } catch (...) {
            warnRateLimited("log entries could not be written");
        }
    }

    void FlushAll() override {}
    void Flush(duckdb::LoggingTargetTable) override {}
    bool IsEnabled(duckdb::LoggingTargetTable) override { return true; }

private:
    void write(const std::string& message, duckdb::optional_idx connection_id) const noexcept {
        try {
            const std::string id = ResolveHttpRequestId(
                database_, connection_id.IsValid() ? std::optional<std::uint64_t>(connection_id.GetIndex())
                                                   : std::nullopt);
            FlapiLogHandler::RequestIdOverride attribute(id);
            CROW_LOG_INFO << DuckDBHttpLogLineFor(message);
        } catch (...) {
            warnRateLimited("a log entry could not be written");
        }
    }

    const void* database_;
};

duckdb::DatabaseInstance* instanceOf(duckdb_connection conn) {
    auto* connection = reinterpret_cast<duckdb::Connection*>(conn);
    if (connection == nullptr || !connection->context || !connection->context->db) {
        return nullptr;
    }
    return connection->context->db.get();
}

}  // namespace

// ---- the switch -----------------------------------------------------------

std::optional<bool> HttpRequestLogEnvSwitch() {
    const char* raw = std::getenv("FLAPI_LOG_HTTP_REQUESTS");
    if (raw == nullptr) {
        return std::nullopt;
    }
    const std::string value = lowered(raw);
    if (value == "1" || value == "true" || value == "yes" || value == "on") {
        return true;
    }
    if (value == "0" || value == "false" || value == "no" || value == "off") {
        return false;
    }
    static std::once_flag warned;
    std::call_once(warned, [] {
        CROW_LOG_WARNING << "FLAPI_LOG_HTTP_REQUESTS must be 1/true/yes/on or 0/false/no/off; ignoring it";
    });
    return std::nullopt;
}

bool ResolveHttpRequestLog(bool config_value) {
    const bool enabled = HttpRequestLogEnvSwitch().value_or(config_value);
    g_resolved.store(enabled ? 1 : 0, std::memory_order_release);
    return enabled;
}

void ResetHttpRequestLogForTest() {
    g_resolved.store(-1, std::memory_order_release);
}

std::string ResolveHttpRequestId(const void* database, std::optional<std::uint64_t> connection_id) {
    // The thread's own request first: a file handle opened by another connection
    // logs through THAT connection's context, so the map could name the wrong
    // request. The map is for threads with no request: DuckDB's workers.
    if (const RequestContext* rc = RequestContextScope::current()) {
        return std::string(rc->requestIdView());
    }
    if (connection_id) {
        if (auto id = LookupHttpRequestAttribution(database, *connection_id)) {
            return std::move(*id);
        }
    }
    return "-";
}

bool HttpRequestLogEnabled() {
    const int resolved = g_resolved.load(std::memory_order_acquire);
    if (resolved >= 0) {
        return resolved == 1;
    }
    return HttpRequestLogEnvSwitch().value_or(false);
}

void EnableDuckDBHttpRequestLog(duckdb_database db) {
    duckdb_connection conn = nullptr;
    if (db == nullptr || duckdb_connect(db, &conn) != DuckDBSuccess) {
        CROW_LOG_WARNING << "duckdb http: no connection to enable the HTTP request log on";
        return;
    }
    try {
        auto* instance = instanceOf(conn);
        if (instance == nullptr) {
            throw std::runtime_error("no database instance");
        }
        auto& log_manager = instance->GetLogManager();
        duckdb::shared_ptr<duckdb::LogSink> sink = duckdb::make_shared_ptr<DuckDBHttpLogSink>(instance);
        log_manager.RegisterLogSink(kSinkName, sink);   // false when already registered: kept
        log_manager.SetLogSink(*instance, kSinkName);
        duckdb::vector<std::string> types{duckdb::HTTPLogType::NAME};
        log_manager.SetEnableStructuredLoggers(types);
        auto* connection = reinterpret_cast<duckdb::Connection*>(conn);
        auto result = connection->Query("SET GLOBAL redact_http_logs = true");
        if (result->HasError()) {
            throw std::runtime_error(result->GetError());
        }
    } catch (const std::exception& e) {
        CROW_LOG_WARNING << "duckdb http: could not enable the HTTP request log: " << RedactUrlsInText(e.what());
    }
    duckdb_disconnect(&conn);
}

// ---- request attribution --------------------------------------------------

HttpRequestAttributionGuard::HttpRequestAttributionGuard(duckdb_connection conn) {
    if (conn == nullptr || g_resolved.load(std::memory_order_relaxed) != 1) {
        return;
    }
    const RequestContext* rc = RequestContextScope::current();
    if (rc == nullptr) {
        return;
    }
    auto* connection = reinterpret_cast<duckdb::Connection*>(conn);
    if (!connection->context) {
        return;
    }
    database_ = connection->context->db.get();
    connection_id_ = connection->context->GetConnectionId();
    std::lock_guard<std::mutex> lock(g_attribution_mutex);
    registered_ = g_attribution.emplace(AttributionKey{database_, connection_id_},
                                        std::string(rc->requestIdView())).second;
}

HttpRequestAttributionGuard::HttpRequestAttributionGuard(const void* database, std::uint64_t connection_id,
                                                         std::string request_id)
    : database_(database), connection_id_(connection_id) {
    std::lock_guard<std::mutex> lock(g_attribution_mutex);
    registered_ = g_attribution.emplace(AttributionKey{database_, connection_id_}, std::move(request_id)).second;
}

HttpRequestAttributionGuard::~HttpRequestAttributionGuard() {
    if (!registered_) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_attribution_mutex);
    g_attribution.erase(AttributionKey{database_, connection_id_});
}

std::optional<std::string> LookupHttpRequestAttribution(const void* database, std::uint64_t connection_id) {
    std::lock_guard<std::mutex> lock(g_attribution_mutex);
    const auto it = g_attribution.find(AttributionKey{database, connection_id});
    if (it == g_attribution.end()) {
        return std::nullopt;
    }
    return it->second;
}

// ---- formatting -----------------------------------------------------------

std::optional<DuckDBHttpLogEntry> ParseDuckDBHttpLogMessage(const std::string& message) {
    const auto parsed = duckdb::Value(message).DefaultTryCastAs(duckdb::HTTPLogType::GetLogType(), nullptr, true);
    if (!parsed) {
        return std::nullopt;
    }
    const duckdb::Value& value = *parsed;
    const auto* request = childNamed(value, "request");
    if (request == nullptr || request->IsNull()) {
        return std::nullopt;
    }
    DuckDBHttpLogEntry entry;
    if (const auto* type = childNamed(*request, "type"); type != nullptr && !type->IsNull()) {
        // RequestType's enum name: GET_REQUEST, HEAD_REQUEST, ...
        entry.method = type->ToString();
        constexpr std::string_view suffix = "_REQUEST";
        if (entry.method.size() > suffix.size() &&
            entry.method.compare(entry.method.size() - suffix.size(), suffix.size(), suffix) == 0) {
            entry.method.erase(entry.method.size() - suffix.size());
        }
    }
    if (entry.method.empty()) {
        return std::nullopt;
    }
    if (const auto* url = childNamed(*request, "url"); url != nullptr && !url->IsNull()) {
        entry.url = url->ToString();
    }
    if (const auto* ms = childNamed(*request, "duration_ms"); ms != nullptr && !ms->IsNull()) {
        entry.duration_ms = ms->GetValue<int64_t>();
    }
    const auto* response = childNamed(value, "response");
    entry.status = response == nullptr ? "ERR" : statusOf(*response);
    return entry;
}

std::string SanitizeLoggedHttpUrl(const std::string& url) {
    // What DuckDB's own BuildLogURL does when it redacts, applied whatever DuckDB
    // was told: a template can `SET redact_http_logs = false` at any time.
    std::string kept;
    std::size_t rest_start = 0;
    if (const auto scheme_end = url.find("://"); scheme_end != std::string::npos) {
        const auto authority_start = scheme_end + 3;
        auto authority_end = url.find_first_of("/?#", authority_start);
        if (authority_end == std::string::npos) {
            authority_end = url.size();
        }
        const std::string authority = url.substr(authority_start, authority_end - authority_start);
        const auto at = authority.rfind('@');   // userinfo: everything before the last '@'
        kept = url.substr(0, authority_start) + (at == std::string::npos ? authority : authority.substr(at + 1));
        rest_start = authority_end;
    }
    std::string rest = url.substr(rest_start);
    rest = rest.substr(0, rest.find('#'));
    if (const auto q = rest.find('?'); q != std::string::npos) {
        rest = rest.substr(0, q) + "?redacted";   // the WHOLE query: no denylist to miss a key
    }
    kept += rest;
    // One token on one line: nothing that ends the line, separates a field or
    // quotes; and only ASCII, so the JSON format stays valid UTF-8.
    static const char* const hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(kept.size());
    for (const char ch : kept) {
        const auto c = static_cast<unsigned char>(ch);
        if (c < 0x21 || c >= 0x7f || c == '"') {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0x0f];
        } else {
            out += ch;
        }
    }
    return out;
}

std::string DuckDBHttpLogLineFor(const std::string& message) {
    if (const auto entry = ParseDuckDBHttpLogMessage(message)) {
        return FormatDuckDBHttpLogLine(*entry);
    }
    // Still a line, or a message that does not cast would hide the request.
    // Nothing from the message: it may carry credentials.
    return kUnparsedLine;
}

std::string FormatDuckDBHttpLogLine(const DuckDBHttpLogEntry& entry) {
    std::string line = "duckdb http ";
    line += entry.method;
    line += ' ';
    line += entry.url.empty() ? "-" : SanitizeLoggedHttpUrl(entry.url);
    line += " -> ";
    line += entry.status;
    line += ' ';
    line += std::to_string(entry.duration_ms < 0 ? 0 : entry.duration_ms);
    line += "ms";
    return line;
}

}  // namespace flapi
