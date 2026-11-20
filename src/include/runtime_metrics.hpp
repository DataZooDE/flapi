#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <crow/json.h>

namespace flapi {

/// Per-endpoint request counters and a fixed-bucket latency histogram,
/// surfaced on GET /api/v1/_config/metrics, `flapii metrics` and the
/// `flapi_get_metrics` config tool.
///
/// Recording is on the request hot path, so it is lock-light and
/// allocation-free once an endpoint has been seen: the series map is read
/// under a shared lock with a heterogeneous (string_view) lookup, and every
/// counter is a relaxed atomic. A series is created on the first request to an
/// endpoint - the only allocating path, and bounded by the configured
/// endpoints because callers pass route TEMPLATES (or a fixed literal for
/// unmatched paths), never filled paths.
///
/// Percentiles are estimated from the histogram: the quantile's bucket is
/// found, and the value interpolated linearly between that bucket's bounds by
/// the quantile's rank inside it, then clamped to the observed maximum. An
/// estimate with bucket resolution (~25% wide buckets), not an exact sample.
/// With the old 1-2.5-5 bounds and the bucket's UPPER bound reported, every
/// request between 100 and 250 ms gave p50 = p95 = p99 = max.
class RuntimeMetrics {
public:
    /// Upper bucket bounds in microseconds (the R10 series: ten per decade,
    /// 1-1.25-1.6-2-2.5-3.2-4-5-6.3-8), 100 us to 60 s; the last bucket is +Inf.
    static constexpr std::array<int64_t, 58> kBucketBoundsUs = {
        100, 125, 160, 200, 250, 320, 400, 500, 630, 800,
        1'000, 1'250, 1'600, 2'000, 2'500, 3'200, 4'000, 5'000, 6'300, 8'000,
        10'000, 12'500, 16'000, 20'000, 25'000, 32'000, 40'000, 50'000, 63'000, 80'000,
        100'000, 125'000, 160'000, 200'000, 250'000, 320'000, 400'000, 500'000, 630'000, 800'000,
        1'000'000, 1'250'000, 1'600'000, 2'000'000, 2'500'000, 3'200'000, 4'000'000, 5'000'000,
        6'300'000, 8'000'000,
        10'000'000, 12'500'000, 16'000'000, 20'000'000, 25'000'000, 32'000'000, 40'000'000, 60'000'000};
    static constexpr std::size_t kBucketCount = kBucketBoundsUs.size() + 1;

    struct Series {
        std::atomic<int64_t> requests{0};
        std::atomic<int64_t> status_2xx{0};
        std::atomic<int64_t> status_3xx{0};
        std::atomic<int64_t> status_4xx{0};
        std::atomic<int64_t> status_5xx{0};
        /// 503s that mean "this endpoint's cache is not ready" (cache_warming,
        /// cache_not_built, cache_build_failed). Counted apart from status_5xx:
        /// they are an expected state during warm-up, not a server fault.
        std::atomic<int64_t> cache_unavailable{0};
        std::atomic<int64_t> failures{0};  ///< MCP tool calls that reported !success (not rejections)
        /// Requests refused for authentication/authorization: REST 401/403
        /// (also counted in status_4xx), MCP role denials (NOT in failures -
        /// a denied caller is not a broken tool).
        std::atomic<int64_t> rejected{0};
        std::atomic<int64_t> sum_us{0};
        std::atomic<int64_t> max_us{0};
        std::array<std::atomic<int64_t>, kBucketCount> buckets{};
        std::atomic<int64_t> last_request_unix_ms{0};
    };

    /// A point-in-time copy of one series, for rendering and tests.
    struct SeriesSnapshot {
        std::string protocol;
        std::string method;
        std::string route;
        int64_t requests = 0;
        int64_t status_2xx = 0, status_3xx = 0, status_4xx = 0, status_5xx = 0;
        int64_t cache_unavailable = 0;
        int64_t failures = 0;
        int64_t rejected = 0;
        double avg_ms = 0.0;
        double max_ms = 0.0;
        double p50_ms = 0.0, p95_ms = 0.0, p99_ms = 0.0;
        int64_t last_request_unix_ms = 0;
    };

    static RuntimeMetrics& instance();

    /// One REST request to `route_template` (never a filled path).
    /// `cache_unavailable`: a 503 answered because the endpoint's cache is not
    /// ready - counted under `cache_unavailable`, not under 5xx.
    void recordRest(std::string_view method, std::string_view route_template,
                    int status_code, std::chrono::microseconds duration,
                    bool cache_unavailable = false);

    /// Whether a response is the cache-readiness 503 (body error "cache_*").
    static bool isCacheUnavailableResponse(int status_code, std::string_view body);

    /// One MCP tool call. `success` is the tool result, independent of HTTP status.
    /// `rejected`: the caller was denied (role check), counted as `rejected`
    /// rather than as a failure / server error.
    void recordMcpTool(std::string_view tool_name, bool success, std::chrono::microseconds duration,
                       bool rejected = false);

    /// One async MCP task reaching a terminal state. `duration` is END TO END:
    /// submit to finish, queue wait included - the tool series times a task's
    /// execution only, because a worker has no request in scope. `success` is
    /// false for a failed task. Reported as protocol "mcp_task" and left out of
    /// the totals (the call is already counted by its tool series).
    void recordMcpTask(std::string_view tool_name, bool success, std::chrono::microseconds duration);

    /// Rejections recorded for every request, including those stopped by
    /// middleware before any handler ran.
    void recordRejection(int status_code);

    std::vector<SeriesSnapshot> snapshot() const;
    int64_t rateLimited() const { return _rate_limited.load(std::memory_order_relaxed); }
    int64_t unauthorized() const { return _unauthorized.load(std::memory_order_relaxed); }
    int64_t forbidden() const { return _forbidden.load(std::memory_order_relaxed); }
    int64_t startedUnixMs() const { return _started_unix_ms; }

    /// JSON for the `requests` block of the metrics response. `route_filter`
    /// (empty = all) keeps only series whose route or tool name matches.
    crow::json::wvalue toJson(const std::string& route_filter = "") const;

    /// Quantile `q` of `buckets` in ms, interpolated inside its bucket and
    /// never more than `max_ms` (when >= 0): a bucket bound above the largest
    /// observation is not a latency anything ever took.
    static double percentileMs(const std::array<int64_t, kBucketCount>& buckets, int64_t total, double q,
                               double max_ms = -1.0);
    static std::size_t bucketFor(int64_t duration_us);

    /// Tests only: forget every series and counter.
    void reset();

private:
    RuntimeMetrics();

    using Key = std::pair<std::string, std::string>;  // (protocol+method, route)
    struct KeyLess {
        using is_transparent = void;
        template <typename A, typename B>
        bool operator()(const A& a, const B& b) const {
            const std::string_view a1 = a.first, a2 = a.second, b1 = b.first, b2 = b.second;
            return a1 < b1 || (a1 == b1 && a2 < b2);
        }
    };

    Series& seriesFor(std::string_view kind, std::string_view route);
    static void observe(Series& s, std::chrono::microseconds duration);

    mutable std::shared_mutex _mutex;
    std::map<Key, std::unique_ptr<Series>, KeyLess> _series;
    std::atomic<int64_t> _rate_limited{0};
    std::atomic<int64_t> _unauthorized{0};
    std::atomic<int64_t> _forbidden{0};
    int64_t _started_unix_ms = 0;
};

} // namespace flapi
