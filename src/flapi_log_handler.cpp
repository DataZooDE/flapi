#include "flapi_log_handler.hpp"
#include "vfs_adapter.hpp"

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

// Crow's own `Request:` line, held between the moment Crow logs it and the
// moment RequestContextMiddleware::before_handle has given the request an id.
// See FlapiLogHandler::releasePendingRequestLine().
struct PendingRequestLine {
    bool present = false;
    std::string message;
    crow::LogLevel level = crow::LogLevel::Info;
};
thread_local PendingRequestLine t_pending_request_line;

// See FlapiLogHandler::RequestIdOverride.
thread_local std::string_view t_request_id_override;

bool isCrowRequestLine(const std::string& message) {
    return message.rfind("Request: ", 0) == 0;
}

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

FlapiLogHandler::RequestIdOverride::RequestIdOverride(std::string_view request_id) noexcept
    : previous_(t_request_id_override) {
    t_request_id_override = request_id;
}

FlapiLogHandler::RequestIdOverride::~RequestIdOverride() noexcept {
    t_request_id_override = previous_;
}

std::string_view FlapiLogHandler::currentRequestIdOverride() noexcept {
    return t_request_id_override;
}

std::string FlapiLogHandler::format(const std::string& message, crow::LogLevel level,
                                    const std::string& timestamp,
                                    std::string_view request_id_override) const {
    const RequestContext* rc = RequestContextScope::current();

    // The request id this line belongs to, if any: an explicit override, the
    // ambient request's, or - for Crow's own Response line of a request whose
    // context has already been released - the one finish() left for it.
    std::string_view request_id;
    if (!request_id_override.empty()) {
        request_id = request_id_override;
        if (rc != nullptr && rc->requestIdView() != request_id_override) {
            rc = nullptr;   // another request's trace ids do not belong on this line
        }
    } else if (rc != nullptr) {
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
    if (request_id_override.empty() && rc == nullptr && isCrowResponseLine(message)) {
        // Used once: the next Response line on this thread is another request's.
        t_pending_response_id.clear();
    }
    return out;
}

void FlapiLogHandler::releasePendingRequestLine() {
    if (!t_pending_request_line.present) {
        return;
    }
    // Taken out BEFORE it is logged again: the log() below must not see it as
    // still pending.
    std::string message = std::move(t_pending_request_line.message);
    const crow::LogLevel level = t_pending_request_line.level;
    t_pending_request_line = PendingRequestLine{};
    // Through Crow's own logger so the level filter and the handler apply as
    // they did the first time; the request is ambient now, so format() stamps it.
    crow::logger(level) << message;
}

void FlapiLogHandler::log(const std::string& raw_message, crow::LogLevel level) {
    std::string message = raw_message;   // redacted / moved below (Crow 1.3 passes a const reference)
    // Every log line: a URL must not carry user:pass@, a presigned signature or a token.
    if (message.find("://") != std::string::npos) {
        message = RedactUrlsInText(message);
    }
    if (holding_.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> guard(write_mutex_);
        // Re-checked under the lock: setFormat() may have flushed meanwhile.
        if (holding_.load(std::memory_order_relaxed)) {
            held_.push_back(HeldLine{std::move(message), level, currentTimestamp(),
                                     std::string(t_request_id_override)});
            return;
        }
    }
    // A Request line still held means before_handle never ran for it (an
    // invalid or upgraded request): write it as it came rather than attach it
    // to whichever request this thread serves next.
    if (t_pending_request_line.present) {
        PendingRequestLine stale = std::move(t_pending_request_line);
        t_pending_request_line = PendingRequestLine{};
        const std::string line = format(stale.message, stale.level, currentTimestamp(), {});
        std::lock_guard<std::mutex> guard(write_mutex_);
        std::cerr << line << '\n';
    }
    // Crow logs `Request: ...` before any middleware has run, so no request is
    // ambient yet and the line would carry no id. before_handle releases it, with
    // the id, a few instructions later on this same thread.
    if (isCrowRequestLine(message) && RequestContextScope::current() == nullptr) {
        t_pending_request_line.present = true;
        t_pending_request_line.message = std::move(message);
        t_pending_request_line.level = level;
        return;
    }
    const std::string line = format(message, level, currentTimestamp(), t_request_id_override);
    // Build first, then one write under the lock: partial writes from pooled
    // workers would interleave into unparseable output.
    std::lock_guard<std::mutex> guard(write_mutex_);
    std::cerr << line << '\n';
}

void FlapiLogHandler::setFormat(Format format) {
    std::lock_guard<std::mutex> guard(write_mutex_);
    format_.store(format, std::memory_order_relaxed);
    for (const auto& held : held_) {
        std::cerr << this->format(held.message, held.level, held.timestamp, held.request_id_override) << '\n';
    }
    held_.clear();
    held_.shrink_to_fit();
    holding_.store(false, std::memory_order_release);
}

}  // namespace flapi
