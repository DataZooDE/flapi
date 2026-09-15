#include "flapi_log_handler.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>

#include <crow/json.h>

#include "request_context.hpp"

namespace flapi {

namespace {

std::string nowIso8601() {
    const auto now = std::chrono::system_clock::now();
    const auto secs = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&secs, &tm);
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

}  // namespace

FlapiLogHandler::FlapiLogHandler(Format format) : format_(format) {}

const char* FlapiLogHandler::levelName(crow::LogLevel level) {
    switch (level) {
        case crow::LogLevel::Debug:    return "debug";
        case crow::LogLevel::Info:     return "info";
        case crow::LogLevel::Warning:  return "warning";
        case crow::LogLevel::Error:    return "error";
        case crow::LogLevel::Critical: return "critical";
        default:                       return "info";
    }
}

std::string FlapiLogHandler::format(const std::string& message, crow::LogLevel level) const {
    const RequestContext* rc = RequestContextScope::current();

    if (format_ == Format::Json) {
        // crow::json::wvalue handles escaping, so a message containing a newline
        // or a quote cannot split or corrupt the record.
        crow::json::wvalue line;
        line["timestamp"] = nowIso8601();
        line["level"] = levelName(level);
        line["message"] = message;
        if (rc != nullptr) {
            line["request_id"] = std::string(rc->requestIdView());
            if (rc->hasTrace()) {
                line["trace_id"] = std::string(rc->traceIdView());
                line["span_id"] = std::string(rc->spanIdView());
            }
        }
        return line.dump();
    }

    std::string out;
    out.reserve(message.size() + 96);
    out += "(";
    out += nowIso8601();
    out += ") [";
    out += levelName(level);
    out += "] ";
    out += message;
    if (rc != nullptr) {
        out += " request_id=";
        out.append(rc->requestIdView());
        if (rc->hasTrace()) {
            out += " trace_id=";
            out.append(rc->traceIdView());
            out += " span_id=";
            out.append(rc->spanIdView());
        }
    }
    return out;
}

void FlapiLogHandler::log(std::string message, crow::LogLevel level) {
    const std::string line = format(message, level);
    // Build first, then one write under the lock: partial writes from pooled
    // workers would interleave into unparseable output.
    std::lock_guard<std::mutex> guard(write_mutex_);
    std::cerr << line << '\n';
}

}  // namespace flapi
