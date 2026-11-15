#include "flapi_log_handler.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>

#include <crow/json.h>

#include "request_context.hpp"
#include "time_utils.hpp"

namespace flapi {

namespace {

// See noteRequestFinished(). Per thread, because Crow logs a request's
// Response line on the thread that completes it.
thread_local const RequestContext* t_response_logged_for = nullptr;
thread_local std::string t_pending_response_id;

bool isCrowResponseLine(const std::string& message) {
    return message.rfind("Response: ", 0) == 0;
}

}  // namespace

FlapiLogHandler::FlapiLogHandler(Format format) : FlapiLogHandler(format, false) {}

FlapiLogHandler::FlapiLogHandler(Format format, bool hold) : format_(format), holding_(hold) {}

FlapiLogHandler::~FlapiLogHandler() {
    if (holding_.load(std::memory_order_acquire)) {
        setFormat(format_.load(std::memory_order_relaxed));
    }
}

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

std::string FlapiLogHandler::currentTimestamp() {
    return nowIso8601Utc();
}

void FlapiLogHandler::noteRequestFinished(const RequestContext* rc, std::string_view request_id) {
    if (t_response_logged_for != rc) {
        t_pending_response_id.assign(request_id.data(), request_id.size());
    }
    t_response_logged_for = nullptr;
}

void FlapiLogHandler::resetPendingResponse() {
    t_pending_response_id.clear();
    t_response_logged_for = nullptr;
}

std::string FlapiLogHandler::format(const std::string& message, crow::LogLevel level,
                                    const std::string& timestamp) const {
    const RequestContext* rc = RequestContextScope::current();

    // The request id this line belongs to, if any: the ambient request's, or -
    // for Crow's own Response line of a request whose context has already been
    // released - the one finish() left for it.
    std::string_view request_id;
    if (rc != nullptr) {
        request_id = rc->requestIdView();
        if (isCrowResponseLine(message)) {
            t_response_logged_for = rc;
        }
    } else if (isCrowResponseLine(message) && !t_pending_response_id.empty()) {
        request_id = t_pending_response_id;
    }

    std::string out;
    if (format_.load(std::memory_order_relaxed) == Format::Json) {
        // crow::json::wvalue handles escaping, so a message containing a newline
        // or a quote cannot split or corrupt the record.
        crow::json::wvalue line;
        line["timestamp"] = timestamp;
        line["level"] = levelName(level);
        line["message"] = message;
        if (!request_id.empty()) {
            line["request_id"] = std::string(request_id);
        }
        if (rc != nullptr && rc->hasTrace()) {
            line["trace_id"] = std::string(rc->traceIdView());
            line["span_id"] = std::string(rc->spanIdView());
        }
        out = line.dump();
    } else {
        out.reserve(message.size() + 96);
        out += "(";
        out += timestamp;
        out += ") [";
        out += levelName(level);
        out += "] ";
        out += message;
        if (!request_id.empty()) {
            out += " request_id=";
            out.append(request_id);
        }
        if (rc != nullptr && rc->hasTrace()) {
            out += " trace_id=";
            out.append(rc->traceIdView());
            out += " span_id=";
            out.append(rc->spanIdView());
        }
    }
    if (rc == nullptr && isCrowResponseLine(message)) {
        // Used once: the next Response line on this thread is another request's.
        t_pending_response_id.clear();
    }
    return out;
}

void FlapiLogHandler::log(std::string message, crow::LogLevel level) {
    if (holding_.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> guard(write_mutex_);
        // Re-checked under the lock: setFormat() may have flushed meanwhile.
        if (holding_.load(std::memory_order_relaxed)) {
            held_.push_back(HeldLine{std::move(message), level, currentTimestamp()});
            return;
        }
    }
    const std::string line = format(message, level, currentTimestamp());
    // Build first, then one write under the lock: partial writes from pooled
    // workers would interleave into unparseable output.
    std::lock_guard<std::mutex> guard(write_mutex_);
    std::cerr << line << '\n';
}

void FlapiLogHandler::setFormat(Format format) {
    std::lock_guard<std::mutex> guard(write_mutex_);
    format_.store(format, std::memory_order_relaxed);
    for (const auto& held : held_) {
        std::cerr << this->format(held.message, held.level, held.timestamp) << '\n';
    }
    held_.clear();
    held_.shrink_to_fit();
    holding_.store(false, std::memory_order_release);
}

}  // namespace flapi
