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

bool RuntimeMetrics::isCacheUnavailableResponse(int status_code, std::string_view body) {
    // CacheManager::readinessBlockJson: {"error":"cache_warming"|"cache_not_built"|
    // "cache_build_failed", ...}. Only ever on a 503.
    return status_code == 503 && body.find(R"("error":"cache_)") != std::string_view::npos;
}

void RuntimeMetrics::recordRest(std::string_view method, std::string_view route_template,
                                int status_code, std::chrono::microseconds duration,
                                bool cache_unavailable) {
    Series& s = seriesFor(method, route_template);
    observe(s, duration);
    if (cache_unavailable) {
        // Not a 5xx: a cache that is still warming (or was never built) is a
        // known, temporary state, and counting it as a server error made
        // `flapii metrics` report failures for every request during warm-up.
        s.cache_unavailable.fetch_add(1, std::memory_order_relaxed);
    } else if (status_code >= 500) {
        s.status_5xx.fetch_add(1, std::memory_order_relaxed);
    } else if (status_code >= 400) {
        s.status_4xx.fetch_add(1, std::memory_order_relaxed);
        if (status_code == 401 || status_code == 403) {
            s.rejected.fetch_add(1, std::memory_order_relaxed);
        }
    } else if (status_code >= 300) {
        s.status_3xx.fetch_add(1, std::memory_order_relaxed);
    } else {
        s.status_2xx.fetch_add(1, std::memory_order_relaxed);
    }
}

void RuntimeMetrics::recordMcpTool(std::string_view tool_name, bool success, std::chrono::microseconds duration,
                                   bool rejected) {
    Series& s = seriesFor(kMcpToolKind, tool_name);
    observe(s, duration);
    if (rejected) {
        // A role denial is the policy working, not the tool failing: it was
        // counted as a failure and therefore as a server error.
        s.rejected.fetch_add(1, std::memory_order_relaxed);
    } else if (!success) {
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

double RuntimeMetrics::percentileMs(const std::array<int64_t, kBucketCount>& buckets, int64_t total, double q,
                                    double max_ms) {
    if (total <= 0) {
        return 0.0;
    }
    // Interpolated inside the bucket by the quantile's rank in it, then
    // clamped to the observed maximum, so a p99 of 250 ms is never reported
    // beside a max of 120 ms. Reporting the bucket's upper bound made p50, p95
    // and p99 identical whenever the traffic sat in one bucket.
    const auto clamp = [max_ms](double v) { return max_ms >= 0.0 ? std::min(v, max_ms) : v; };
    const double rank = q * static_cast<double>(total);
    int64_t cumulative = 0;
    for (std::size_t i = 0; i < kBucketCount; ++i) {
        const int64_t before = cumulative;
        cumulative += buckets[i];
        if (buckets[i] > 0 && static_cast<double>(cumulative) >= rank) {
            // The +Inf bucket has no upper bound: the observed maximum is the
            // best estimate (the largest finite bound without one).
            if (i >= kBucketBoundsUs.size()) {
                return max_ms >= 0.0 ? max_ms : static_cast<double>(kBucketBoundsUs.back()) / 1000.0;
            }
            const double lower = i == 0 ? 0.0 : static_cast<double>(kBucketBoundsUs[i - 1]);
            const double upper = static_cast<double>(kBucketBoundsUs[i]);
            const double fraction =
                std::clamp((rank - static_cast<double>(before)) / static_cast<double>(buckets[i]), 0.0, 1.0);
            return clamp((lower + (upper - lower) * fraction) / 1000.0);
        }
    }
    return clamp(static_cast<double>(kBucketBoundsUs.back()) / 1000.0);
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
        snap.cache_unavailable = series->cache_unavailable.load(std::memory_order_relaxed);
        snap.failures = series->failures.load(std::memory_order_relaxed);
        snap.rejected = series->rejected.load(std::memory_order_relaxed);
        std::array<int64_t, kBucketCount> buckets{};
        int64_t total = 0;
        for (std::size_t i = 0; i < kBucketCount; ++i) {
            buckets[i] = series->buckets[i].load(std::memory_order_relaxed);
            total += buckets[i];
        }
        const int64_t sum_us = series->sum_us.load(std::memory_order_relaxed);
        snap.avg_ms = total > 0 ? static_cast<double>(sum_us) / static_cast<double>(total) / 1000.0 : 0.0;
        snap.max_ms = static_cast<double>(series->max_us.load(std::memory_order_relaxed)) / 1000.0;
        snap.p50_ms = percentileMs(buckets, total, 0.50, snap.max_ms);
        snap.p95_ms = percentileMs(buckets, total, 0.95, snap.max_ms);
        snap.p99_ms = percentileMs(buckets, total, 0.99, snap.max_ms);
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
    int64_t total = 0, errors = 0, cache_unavailable = 0;
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
        row["rejected"] = s.rejected;
        if (s.protocol == "mcp") {
            row["failures"] = s.failures;
        } else {
            row["status"]["2xx"] = s.status_2xx;
            row["status"]["3xx"] = s.status_3xx;
            row["status"]["4xx"] = s.status_4xx;
            row["status"]["5xx"] = s.status_5xx;
            row["status"]["503_cache"] = s.cache_unavailable;
        }
        row["latency_ms"]["avg"] = s.avg_ms;
        row["latency_ms"]["p50"] = s.p50_ms;
        row["latency_ms"]["p95"] = s.p95_ms;
        row["latency_ms"]["p99"] = s.p99_ms;
        row["latency_ms"]["max"] = s.max_ms;
        row["last_request_unix_ms"] = s.last_request_unix_ms;
        total += s.requests;
        // server_errors excludes cache-not-ready 503s; they are cache_unavailable.
        errors += s.protocol == "mcp" ? s.failures : s.status_5xx;
        cache_unavailable += s.cache_unavailable;
        rows.push_back(std::move(row));
    }
    out["total"] = total;
    out["server_errors"] = errors;
    out["cache_unavailable"] = cache_unavailable;
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
