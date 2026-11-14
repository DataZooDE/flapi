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
/// Percentiles are estimated from the histogram: the reported value is the
/// upper bound of the bucket that contains the quantile, so it is an upper
/// estimate with bucket resolution, not an exact sample.
class RuntimeMetrics {
public:
    /// Upper bucket bounds in microseconds; the last bucket is +Inf.
    static constexpr std::array<int64_t, 18> kBucketBoundsUs = {
        100, 250, 500, 1'000, 2'500, 5'000, 10'000, 25'000, 50'000, 100'000,
        250'000, 500'000, 1'000'000, 2'500'000, 5'000'000, 10'000'000, 30'000'000, 60'000'000};
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
        std::atomic<int64_t> failures{0};  ///< MCP tool calls that reported !success
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
    void recordMcpTool(std::string_view tool_name, bool success, std::chrono::microseconds duration);

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

    /// Upper bound (ms) of the bucket containing quantile `q` of `buckets`,
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
