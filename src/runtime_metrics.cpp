#include "runtime_metrics.hpp"

#include <algorithm>
#include <mutex>

namespace flapi {

namespace {

// The series kind for MCP tool calls. REST series use the HTTP method, which
// is never this literal, so the two protocols cannot collide on one key.
constexpr std::string_view kMcpToolKind = "TOOL";

int64_t nowUnixMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

void atomicMax(std::atomic<int64_t>& target, int64_t value) {
    int64_t current = target.load(std::memory_order_relaxed);
    while (value > current &&
           !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
    }
}

} // namespace

RuntimeMetrics& RuntimeMetrics::instance() {
    static RuntimeMetrics metrics;
    return metrics;
}

RuntimeMetrics::RuntimeMetrics() : _started_unix_ms(nowUnixMs()) {}

std::size_t RuntimeMetrics::bucketFor(int64_t duration_us) {
    const auto it = std::lower_bound(kBucketBoundsUs.begin(), kBucketBoundsUs.end(), duration_us);
    return static_cast<std::size_t>(it - kBucketBoundsUs.begin());
}

RuntimeMetrics::Series& RuntimeMetrics::seriesFor(std::string_view kind, std::string_view route) {
    const std::pair<std::string_view, std::string_view> probe{kind, route};
    {
        std::shared_lock lock(_mutex);
        const auto it = _series.find(probe);
        if (it != _series.end()) {
            return *it->second;
        }
    }
    std::unique_lock lock(_mutex);
    auto it = _series.find(probe);
    if (it == _series.end()) {
        it = _series.emplace(Key{std::string(kind), std::string(route)}, std::make_unique<Series>()).first;
    }
    return *it->second;
}

void RuntimeMetrics::observe(Series& s, std::chrono::microseconds duration) {
    const int64_t us = std::max<int64_t>(0, duration.count());
    s.requests.fetch_add(1, std::memory_order_relaxed);
    s.sum_us.fetch_add(us, std::memory_order_relaxed);
    atomicMax(s.max_us, us);
    s.buckets[bucketFor(us)].fetch_add(1, std::memory_order_relaxed);
    s.last_request_unix_ms.store(nowUnixMs(), std::memory_order_relaxed);
}

void RuntimeMetrics::recordRest(std::string_view method, std::string_view route_template,
                                int status_code, std::chrono::microseconds duration) {
    Series& s = seriesFor(method, route_template);
    observe(s, duration);
    if (status_code >= 500) {
        s.status_5xx.fetch_add(1, std::memory_order_relaxed);
    } else if (status_code >= 400) {
        s.status_4xx.fetch_add(1, std::memory_order_relaxed);
    } else if (status_code >= 300) {
        s.status_3xx.fetch_add(1, std::memory_order_relaxed);
    } else {
        s.status_2xx.fetch_add(1, std::memory_order_relaxed);
    }
}

void RuntimeMetrics::recordMcpTool(std::string_view tool_name, bool success, std::chrono::microseconds duration) {
    Series& s = seriesFor(kMcpToolKind, tool_name);
    observe(s, duration);
    if (!success) {
        s.failures.fetch_add(1, std::memory_order_relaxed);
    }
}

void RuntimeMetrics::recordRejection(int status_code) {
    if (status_code == 429) {
        _rate_limited.fetch_add(1, std::memory_order_relaxed);
    } else if (status_code == 401) {
        _unauthorized.fetch_add(1, std::memory_order_relaxed);
    } else if (status_code == 403) {
        _forbidden.fetch_add(1, std::memory_order_relaxed);
    }
}

double RuntimeMetrics::percentileMs(const std::array<int64_t, kBucketCount>& buckets, int64_t total, double q) {
    if (total <= 0) {
        return 0.0;
    }
    const auto rank = static_cast<int64_t>(q * static_cast<double>(total) + 0.999999);
    int64_t cumulative = 0;
    for (std::size_t i = 0; i < kBucketCount; ++i) {
        cumulative += buckets[i];
        if (cumulative >= rank) {
            // The +Inf bucket has no upper bound; report the largest finite one.
            const int64_t bound = i < kBucketBoundsUs.size() ? kBucketBoundsUs[i] : kBucketBoundsUs.back();
            return static_cast<double>(bound) / 1000.0;
        }
    }
    return static_cast<double>(kBucketBoundsUs.back()) / 1000.0;
}

std::vector<RuntimeMetrics::SeriesSnapshot> RuntimeMetrics::snapshot() const {
    std::vector<SeriesSnapshot> out;
    std::shared_lock lock(_mutex);
    out.reserve(_series.size());
    for (const auto& [key, series] : _series) {
        SeriesSnapshot snap;
        const bool is_tool = key.first == kMcpToolKind;
        snap.protocol = is_tool ? "mcp" : "rest";
        snap.method = is_tool ? "" : key.first;
        snap.route = key.second;
        snap.requests = series->requests.load(std::memory_order_relaxed);
        snap.status_2xx = series->status_2xx.load(std::memory_order_relaxed);
        snap.status_3xx = series->status_3xx.load(std::memory_order_relaxed);
        snap.status_4xx = series->status_4xx.load(std::memory_order_relaxed);
        snap.status_5xx = series->status_5xx.load(std::memory_order_relaxed);
        snap.failures = series->failures.load(std::memory_order_relaxed);
        std::array<int64_t, kBucketCount> buckets{};
        int64_t total = 0;
        for (std::size_t i = 0; i < kBucketCount; ++i) {
            buckets[i] = series->buckets[i].load(std::memory_order_relaxed);
            total += buckets[i];
        }
        const int64_t sum_us = series->sum_us.load(std::memory_order_relaxed);
        snap.avg_ms = total > 0 ? static_cast<double>(sum_us) / static_cast<double>(total) / 1000.0 : 0.0;
        snap.max_ms = static_cast<double>(series->max_us.load(std::memory_order_relaxed)) / 1000.0;
        snap.p50_ms = percentileMs(buckets, total, 0.50);
        snap.p95_ms = percentileMs(buckets, total, 0.95);
        snap.p99_ms = percentileMs(buckets, total, 0.99);
        snap.last_request_unix_ms = series->last_request_unix_ms.load(std::memory_order_relaxed);
        out.push_back(std::move(snap));
    }
    return out;
}

crow::json::wvalue RuntimeMetrics::toJson(const std::string& route_filter) const {
    crow::json::wvalue out;
    out["since_unix_ms"] = _started_unix_ms;
    out["rejections"]["rate_limited"] = rateLimited();
    out["rejections"]["unauthorized"] = unauthorized();
    out["rejections"]["forbidden"] = forbidden();

    std::vector<crow::json::wvalue> rows;
    int64_t total = 0, errors = 0;
    for (const auto& s : snapshot()) {
        if (!route_filter.empty() && s.route != route_filter) {
            continue;
        }
        crow::json::wvalue row;
        row["protocol"] = s.protocol;
        if (!s.method.empty()) {
            row["method"] = s.method;
        }
        row[s.protocol == "mcp" ? "tool" : "route"] = s.route;
        row["requests"] = s.requests;
        if (s.protocol == "mcp") {
            row["failures"] = s.failures;
        } else {
            row["status"]["2xx"] = s.status_2xx;
            row["status"]["3xx"] = s.status_3xx;
            row["status"]["4xx"] = s.status_4xx;
            row["status"]["5xx"] = s.status_5xx;
        }
        row["latency_ms"]["avg"] = s.avg_ms;
        row["latency_ms"]["p50"] = s.p50_ms;
        row["latency_ms"]["p95"] = s.p95_ms;
        row["latency_ms"]["p99"] = s.p99_ms;
        row["latency_ms"]["max"] = s.max_ms;
        row["last_request_unix_ms"] = s.last_request_unix_ms;
        total += s.requests;
        errors += s.protocol == "mcp" ? s.failures : s.status_5xx;
        rows.push_back(std::move(row));
    }
    out["total"] = total;
    out["server_errors"] = errors;
    out["series"] = crow::json::wvalue(rows);
    return out;
}

void RuntimeMetrics::reset() {
    std::unique_lock lock(_mutex);
    _series.clear();
    _rate_limited.store(0);
    _unauthorized.store(0);
    _forbidden.store(0);
    _started_unix_ms = nowUnixMs();
}

} // namespace flapi
