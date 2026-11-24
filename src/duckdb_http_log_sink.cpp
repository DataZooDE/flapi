#include "duckdb_http_log_sink.hpp"

#include <algorithm>
#include <atomic>
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

class DuckDBHttpLogSink : public duckdb::LogSink {
public:
    explicit DuckDBHttpLogSink(const void* database) : database_(database) {}

    const std::string GetSinkName() override { return kSinkName; }

    // Called by the LogManager under its own lock, synchronously, on the thread
    // that made the request. Nothing is buffered: the line is in the flAPI log
    // when DuckDB returns from the request.
    void WriteLogEntry(duckdb::timestamp_t, duckdb::LogLevel, const std::string& log_type,
                       const std::string& log_message, const duckdb::RegisteredLoggingContext& context) override {
        if (log_type != duckdb::HTTPLogType::NAME) {
            return;
        }
        write(log_message, context.context.connection_id);
    }

    void WriteLogEntries(duckdb::DataChunk& chunk, const duckdb::RegisteredLoggingContext& context) override {
        // LOG_ENTRIES layout: context_id, timestamp, type, log_level, message.
        for (duckdb::idx_t row = 0; row < chunk.size(); ++row) {
            const auto type = chunk.GetValue(2, row);
            const auto message = chunk.GetValue(4, row);
            if (type.IsNull() || message.IsNull() || type.ToString() != duckdb::HTTPLogType::NAME) {
                continue;
            }
            write(message.ToString(), context.context.connection_id);
        }
    }

    void FlushAll() override {}
    void Flush(duckdb::LoggingTargetTable) override {}
    bool IsEnabled(duckdb::LoggingTargetTable) override { return true; }

private:
    void write(const std::string& message, duckdb::optional_idx connection_id) const {
        const auto entry = ParseDuckDBHttpLogMessage(message);
        if (!entry) {
            // Never the raw message: with redaction off it carries the URL's credentials.
            CROW_LOG_WARNING << "duckdb http: a log entry did not parse as DuckDB's HTTP log type";
            return;
        }
        // Whose request is this: the flAPI request running a query on this
        // connection (any thread), else the one ambient on this thread (flAPI's
        // own file reads), else none.
        std::optional<std::string> request_id;
        if (connection_id.IsValid()) {
            request_id = LookupHttpRequestAttribution(database_, connection_id.GetIndex());
        }
        if (!request_id) {
            if (const RequestContext* rc = RequestContextScope::current()) {
                request_id = std::string(rc->requestIdView());
            }
        }
        const std::string id = request_id.value_or("-");
        FlapiLogHandler::RequestIdOverride attribute(id);
        CROW_LOG_INFO << FormatDuckDBHttpLogLine(*entry);
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

std::string FormatDuckDBHttpLogLine(const DuckDBHttpLogEntry& entry) {
    std::string line = "duckdb http ";
    line += entry.method;
    line += ' ';
    line += entry.url.empty() ? "-" : PathSchemeUtils::RedactUrl(entry.url);
    line += " -> ";
    line += entry.status;
    line += ' ';
    line += std::to_string(entry.duration_ms < 0 ? 0 : entry.duration_ms);
    line += "ms";
    return line;
}

}  // namespace flapi
