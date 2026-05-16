#include "audit_logger.hpp"

#include <chrono>
#include <crow/json.h>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>

namespace flapi {

namespace {

std::string randHex(int len) {
    static thread_local std::mt19937_64 rng(std::random_device{}());
    std::uniform_int_distribution<int> dist(0, 15);
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(len);
    for (int i = 0; i < len; ++i) {
        out.push_back(kHex[dist(rng)]);
    }
    return out;
}

} // namespace

AuditLogger::AuditLogger(AuditConfig config) : config_(std::move(config)) {
    if (!config_.enabled) {
        return;
    }
    if (config_.sink == "stdout") {
        sink_stream_ = &std::cout;
    } else if (config_.sink == "file") {
        auto file = std::make_unique<std::ofstream>(config_.path, std::ios::app);
        if (!file->is_open()) {
            throw std::runtime_error("audit log: cannot open " + config_.path);
        }
        file_stream_ = std::move(file);
        sink_stream_ = file_stream_.get();
    } else if (config_.sink == "null") {
        sink_stream_ = nullptr;
    } else {
        throw std::runtime_error("audit log: unknown sink '" + config_.sink + "'");
    }
}

AuditLogger::~AuditLogger() = default;

void AuditLogger::log(AuditEvent event) {
    if (!config_.enabled) {
        return;
    }
    if (event.timestamp.empty()) {
        event.timestamp = nowIso8601();
    }
    if (event.request_id.empty()) {
        event.request_id = generateRequestId();
    }

    const std::string line = serialiseEvent(event);

    if (sink_stream_ == nullptr) {
        return;  // null sink — no I/O
    }

    std::lock_guard<std::mutex> guard(write_mutex_);
    (*sink_stream_) << line << '\n';
    sink_stream_->flush();
}

std::string AuditLogger::nowIso8601() {
    const auto now = std::chrono::system_clock::now();
    const auto now_t = std::chrono::system_clock::to_time_t(now);
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                            now.time_since_epoch()).count() % 1'000'000;
    std::tm tm_buf{};
#ifdef _WIN32
    gmtime_s(&tm_buf, &now_t);
#else
    gmtime_r(&now_t, &tm_buf);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%S")
        << '.' << std::setfill('0') << std::setw(6) << micros << 'Z';
    return oss.str();
}

std::string AuditLogger::generateRequestId() {
    // 16 hex chars — short enough for logs, wide enough to avoid collisions
    // within any realistic flapi deployment.
    return "req-" + randHex(16);
}

std::string AuditLogger::serialiseEvent(const AuditEvent& event) const {
    crow::json::wvalue line;
    line["timestamp"] = event.timestamp;
    line["request_id"] = event.request_id;
    line["principal"] = event.principal;
    line["method"] = event.method;
    line["target"] = event.target;
    line["status"] = event.status;
    line["row_count"] = event.row_count;
    line["latency_ms"] = event.latency_ms;

    crow::json::wvalue params = crow::json::wvalue::object();
    for (const auto& [key, value] : event.params) {
        if (config_.redact_keys.count(key) > 0) {
            params[key] = "<redacted>";
        } else {
            params[key] = value;
        }
    }
    line["params"] = std::move(params);
    return line.dump();
}

} // namespace flapi
