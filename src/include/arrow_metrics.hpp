/**
 * Arrow IPC Metrics and Logging
 *
 * Provides metrics collection and logging for Arrow IPC streaming.
 * Supports:
 * - Counter metrics (requests, batches, bytes, errors)
 * - Gauge metrics (active streams, memory usage)
 * - Request lifecycle logging
 */

#pragma once

#include <atomic>
#include <chrono>
#include <string>
#include <mutex>

namespace flapi {

/**
 * Arrow metrics counters (monotonically increasing).
 */
struct ArrowCounters {
    std::atomic<uint64_t> totalRequests{0};      // Total Arrow serialization requests
    std::atomic<uint64_t> successfulRequests{0}; // Successful serializations
    std::atomic<uint64_t> failedRequests{0};     // Failed serializations
    std::atomic<uint64_t> totalBatches{0};       // Total record batches written
    std::atomic<uint64_t> totalRows{0};          // Total rows serialized
    std::atomic<uint64_t> totalBytesWritten{0};  // Total bytes written (pre-compression)
    std::atomic<uint64_t> totalBytesCompressed{0}; // Total bytes after compression
    std::atomic<uint64_t> compressionRequests{0};  // Requests with compression
    std::atomic<uint64_t> compressionErrors{0};    // Compression failures
    std::atomic<uint64_t> memoryLimitErrors{0};    // Memory limit exceeded errors
};

/**
 * Arrow metrics gauges (current values).
 */
struct ArrowGauges {
    std::atomic<int32_t> activeStreams{0};       // Currently active Arrow streams
    std::atomic<uint64_t> currentMemoryUsage{0}; // Current memory usage estimate
    std::atomic<int32_t> peakActiveStreams{0};   // Peak concurrent streams
    std::atomic<uint64_t> peakMemoryUsage{0};    // Peak memory usage
};

/**
 * Arrow metrics histograms (aggregated statistics).
 * Uses simple min/max/sum for histogram-like data without external dependencies.
 */
struct ArrowHistograms {
    // Duration statistics (microseconds)
    std::atomic<uint64_t> minDurationUs{UINT64_MAX};
    std::atomic<uint64_t> maxDurationUs{0};
    std::atomic<uint64_t> totalDurationUs{0};

    // Batch size statistics
    std::atomic<uint64_t> minBatchRows{UINT64_MAX};
    std::atomic<uint64_t> maxBatchRows{0};

    // Response size statistics (bytes)
    std::atomic<uint64_t> minResponseBytes{UINT64_MAX};
    std::atomic<uint64_t> maxResponseBytes{0};

    // Compression ratio statistics (percentage * 100)
    std::atomic<uint64_t> minCompressionRatio{UINT64_MAX};
    std::atomic<uint64_t> maxCompressionRatio{0};
    std::atomic<uint64_t> totalCompressionRatio{0};
    std::atomic<uint64_t> compressionRatioCount{0};
};

/**
 * Global Arrow metrics collector (singleton).
 */
class ArrowMetrics {
public:
    static ArrowMetrics& instance() {
        static ArrowMetrics metrics;
        return metrics;
    }

    // Counters
    ArrowCounters counters;

    // Gauges
    ArrowGauges gauges;

    // Histograms
    ArrowHistograms histograms;

    /**
     * Record the start of an Arrow serialization request.
     * Returns start time for duration tracking.
     */
    std::chrono::steady_clock::time_point recordRequestStart() {
        counters.totalRequests.fetch_add(1, std::memory_order_relaxed);
        int32_t active = gauges.activeStreams.fetch_add(1, std::memory_order_relaxed) + 1;

        // Update peak if needed
        int32_t peak = gauges.peakActiveStreams.load(std::memory_order_relaxed);
        while (active > peak &&
               !gauges.peakActiveStreams.compare_exchange_weak(peak, active, std::memory_order_relaxed)) {
            // CAS loop
        }

        return std::chrono::steady_clock::now();
    }

    /**
     * Record a successful Arrow serialization request.
     */
    void recordRequestSuccess(
        std::chrono::steady_clock::time_point startTime,
        size_t rows,
        size_t batches,
        size_t bytesWritten,
        size_t bytesCompressed,
        bool wasCompressed
    ) {
        auto endTime = std::chrono::steady_clock::now();
        auto durationUs = std::chrono::duration_cast<std::chrono::microseconds>(
            endTime - startTime).count();

        counters.successfulRequests.fetch_add(1, std::memory_order_relaxed);
        counters.totalBatches.fetch_add(batches, std::memory_order_relaxed);
        counters.totalRows.fetch_add(rows, std::memory_order_relaxed);
        counters.totalBytesWritten.fetch_add(bytesWritten, std::memory_order_relaxed);
        counters.totalBytesCompressed.fetch_add(bytesCompressed, std::memory_order_relaxed);

        if (wasCompressed) {
            counters.compressionRequests.fetch_add(1, std::memory_order_relaxed);

            // Calculate and record compression ratio
            if (bytesWritten > 0) {
                uint64_t ratio = (bytesCompressed * 10000) / bytesWritten; // percentage * 100
                updateHistogramMin(histograms.minCompressionRatio, ratio);
                updateHistogramMax(histograms.maxCompressionRatio, ratio);
                histograms.totalCompressionRatio.fetch_add(ratio, std::memory_order_relaxed);
                histograms.compressionRatioCount.fetch_add(1, std::memory_order_relaxed);
            }
        }

        // Update histograms
        updateHistogramMin(histograms.minDurationUs, static_cast<uint64_t>(durationUs));
        updateHistogramMax(histograms.maxDurationUs, static_cast<uint64_t>(durationUs));
        histograms.totalDurationUs.fetch_add(durationUs, std::memory_order_relaxed);

        updateHistogramMin(histograms.minResponseBytes, bytesCompressed > 0 ? bytesCompressed : bytesWritten);
        updateHistogramMax(histograms.maxResponseBytes, bytesCompressed > 0 ? bytesCompressed : bytesWritten);

        // Decrement active streams
        gauges.activeStreams.fetch_sub(1, std::memory_order_relaxed);
    }

    /**
     * Record a failed Arrow serialization request.
     */
    void recordRequestFailure(
        std::chrono::steady_clock::time_point startTime,
        const std::string& errorType
    ) {
        counters.failedRequests.fetch_add(1, std::memory_order_relaxed);

        if (errorType == "memory") {
            counters.memoryLimitErrors.fetch_add(1, std::memory_order_relaxed);
        } else if (errorType == "compression") {
            counters.compressionErrors.fetch_add(1, std::memory_order_relaxed);
        }

        // Decrement active streams
        gauges.activeStreams.fetch_sub(1, std::memory_order_relaxed);
    }

    /**
     * Record batch statistics.
     */
    void recordBatchStats(size_t rowsInBatch) {
        updateHistogramMin(histograms.minBatchRows, rowsInBatch);
        updateHistogramMax(histograms.maxBatchRows, rowsInBatch);
    }

    /**
     * Update memory usage estimate.
     */
    void updateMemoryUsage(int64_t delta) {
        uint64_t newUsage;
        if (delta >= 0) {
            newUsage = gauges.currentMemoryUsage.fetch_add(
                static_cast<uint64_t>(delta), std::memory_order_relaxed) + delta;
        } else {
            newUsage = gauges.currentMemoryUsage.fetch_sub(
                static_cast<uint64_t>(-delta), std::memory_order_relaxed) - (-delta);
        }

        // Update peak if needed
        uint64_t peak = gauges.peakMemoryUsage.load(std::memory_order_relaxed);
        while (newUsage > peak &&
               !gauges.peakMemoryUsage.compare_exchange_weak(peak, newUsage, std::memory_order_relaxed)) {
            // CAS loop
        }
    }

    /**
     * Reset all metrics (for testing).
     */
    void reset() {
        // Reset counters
        counters.totalRequests.store(0, std::memory_order_relaxed);
        counters.successfulRequests.store(0, std::memory_order_relaxed);
        counters.failedRequests.store(0, std::memory_order_relaxed);
        counters.totalBatches.store(0, std::memory_order_relaxed);
        counters.totalRows.store(0, std::memory_order_relaxed);
        counters.totalBytesWritten.store(0, std::memory_order_relaxed);
        counters.totalBytesCompressed.store(0, std::memory_order_relaxed);
        counters.compressionRequests.store(0, std::memory_order_relaxed);
        counters.compressionErrors.store(0, std::memory_order_relaxed);
        counters.memoryLimitErrors.store(0, std::memory_order_relaxed);

        // Reset gauges
        gauges.activeStreams.store(0, std::memory_order_relaxed);
        gauges.currentMemoryUsage.store(0, std::memory_order_relaxed);
        gauges.peakActiveStreams.store(0, std::memory_order_relaxed);
        gauges.peakMemoryUsage.store(0, std::memory_order_relaxed);

        // Reset histograms
        histograms.minDurationUs.store(UINT64_MAX, std::memory_order_relaxed);
        histograms.maxDurationUs.store(0, std::memory_order_relaxed);
        histograms.totalDurationUs.store(0, std::memory_order_relaxed);
        histograms.minBatchRows.store(UINT64_MAX, std::memory_order_relaxed);
        histograms.maxBatchRows.store(0, std::memory_order_relaxed);
        histograms.minResponseBytes.store(UINT64_MAX, std::memory_order_relaxed);
        histograms.maxResponseBytes.store(0, std::memory_order_relaxed);
        histograms.minCompressionRatio.store(UINT64_MAX, std::memory_order_relaxed);
        histograms.maxCompressionRatio.store(0, std::memory_order_relaxed);
        histograms.totalCompressionRatio.store(0, std::memory_order_relaxed);
        histograms.compressionRatioCount.store(0, std::memory_order_relaxed);
    }

    /**
     * Get average request duration in microseconds.
     */
    double getAverageDurationUs() const {
        uint64_t total = histograms.totalDurationUs.load(std::memory_order_relaxed);
        uint64_t count = counters.successfulRequests.load(std::memory_order_relaxed);
        return count > 0 ? static_cast<double>(total) / count : 0.0;
    }

    /**
     * Get average compression ratio (1.0 = no compression, 0.5 = 50% size).
     */
    double getAverageCompressionRatio() const {
        uint64_t total = histograms.totalCompressionRatio.load(std::memory_order_relaxed);
        uint64_t count = histograms.compressionRatioCount.load(std::memory_order_relaxed);
        return count > 0 ? static_cast<double>(total) / count / 10000.0 : 1.0;
    }

private:
    ArrowMetrics() = default;
    ArrowMetrics(const ArrowMetrics&) = delete;
    ArrowMetrics& operator=(const ArrowMetrics&) = delete;

    static void updateHistogramMin(std::atomic<uint64_t>& minVal, uint64_t newVal) {
        uint64_t current = minVal.load(std::memory_order_relaxed);
        while (newVal < current &&
               !minVal.compare_exchange_weak(current, newVal, std::memory_order_relaxed)) {
            // CAS loop
        }
    }

    static void updateHistogramMax(std::atomic<uint64_t>& maxVal, uint64_t newVal) {
        uint64_t current = maxVal.load(std::memory_order_relaxed);
        while (newVal > current &&
               !maxVal.compare_exchange_weak(current, newVal, std::memory_order_relaxed)) {
            // CAS loop
        }
    }
};

/**
 * RAII helper for tracking Arrow serialization request lifecycle.
 */
class ArrowRequestScope {
public:
    explicit ArrowRequestScope(ArrowMetrics& metrics = ArrowMetrics::instance())
        : _metrics(metrics)
        , _startTime(metrics.recordRequestStart())
        , _completed(false)
    {}

    ~ArrowRequestScope() {
        if (!_completed) {
            // Request abandoned without success/failure being recorded
            _metrics.recordRequestFailure(_startTime, "abandoned");
        }
    }

    void recordSuccess(size_t rows, size_t batches, size_t bytesWritten,
                       size_t bytesCompressed, bool wasCompressed) {
        _metrics.recordRequestSuccess(_startTime, rows, batches, bytesWritten,
                                      bytesCompressed, wasCompressed);
        _completed = true;
    }

    void recordFailure(const std::string& errorType) {
        _metrics.recordRequestFailure(_startTime, errorType);
        _completed = true;
    }

    std::chrono::steady_clock::time_point startTime() const { return _startTime; }

private:
    ArrowMetrics& _metrics;
    std::chrono::steady_clock::time_point _startTime;
    bool _completed;
};

} // namespace flapi
