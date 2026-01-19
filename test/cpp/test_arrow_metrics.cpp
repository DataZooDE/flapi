/**
 * Arrow Metrics Unit Tests
 *
 * Tests for Arrow IPC metrics collection and tracking.
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <thread>
#include <vector>

#include "arrow_metrics.hpp"

using namespace flapi;

TEST_CASE("Arrow Metrics Counters", "[arrow][metrics][counters]") {
    // Reset metrics before each test
    ArrowMetrics::instance().reset();

    SECTION("Initial counter values are zero") {
        auto& metrics = ArrowMetrics::instance();
        REQUIRE(metrics.counters.totalRequests.load() == 0);
        REQUIRE(metrics.counters.successfulRequests.load() == 0);
        REQUIRE(metrics.counters.failedRequests.load() == 0);
        REQUIRE(metrics.counters.totalBatches.load() == 0);
        REQUIRE(metrics.counters.totalRows.load() == 0);
        REQUIRE(metrics.counters.totalBytesWritten.load() == 0);
    }

    SECTION("Request start increments total requests") {
        auto& metrics = ArrowMetrics::instance();

        metrics.recordRequestStart();
        REQUIRE(metrics.counters.totalRequests.load() == 1);

        metrics.recordRequestStart();
        REQUIRE(metrics.counters.totalRequests.load() == 2);
    }

    SECTION("Successful request increments success counter") {
        auto& metrics = ArrowMetrics::instance();

        auto start = metrics.recordRequestStart();
        metrics.recordRequestSuccess(start, 100, 1, 1024, 512, true);

        REQUIRE(metrics.counters.successfulRequests.load() == 1);
        REQUIRE(metrics.counters.totalRows.load() == 100);
        REQUIRE(metrics.counters.totalBatches.load() == 1);
        REQUIRE(metrics.counters.totalBytesWritten.load() == 1024);
        REQUIRE(metrics.counters.totalBytesCompressed.load() == 512);
        REQUIRE(metrics.counters.compressionRequests.load() == 1);
    }

    SECTION("Failed request increments failure counter") {
        auto& metrics = ArrowMetrics::instance();

        auto start = metrics.recordRequestStart();
        metrics.recordRequestFailure(start, "memory");

        REQUIRE(metrics.counters.failedRequests.load() == 1);
        REQUIRE(metrics.counters.memoryLimitErrors.load() == 1);
    }

    SECTION("Compression errors are tracked") {
        auto& metrics = ArrowMetrics::instance();

        auto start = metrics.recordRequestStart();
        metrics.recordRequestFailure(start, "compression");

        REQUIRE(metrics.counters.failedRequests.load() == 1);
        REQUIRE(metrics.counters.compressionErrors.load() == 1);
    }
}

TEST_CASE("Arrow Metrics Gauges", "[arrow][metrics][gauges]") {
    ArrowMetrics::instance().reset();

    SECTION("Active streams gauge tracks concurrent requests") {
        auto& metrics = ArrowMetrics::instance();

        auto start1 = metrics.recordRequestStart();
        REQUIRE(metrics.gauges.activeStreams.load() == 1);

        auto start2 = metrics.recordRequestStart();
        REQUIRE(metrics.gauges.activeStreams.load() == 2);

        metrics.recordRequestSuccess(start1, 10, 1, 100, 100, false);
        REQUIRE(metrics.gauges.activeStreams.load() == 1);

        metrics.recordRequestSuccess(start2, 10, 1, 100, 100, false);
        REQUIRE(metrics.gauges.activeStreams.load() == 0);
    }

    SECTION("Peak active streams is recorded") {
        auto& metrics = ArrowMetrics::instance();

        auto start1 = metrics.recordRequestStart();
        auto start2 = metrics.recordRequestStart();
        auto start3 = metrics.recordRequestStart();

        REQUIRE(metrics.gauges.peakActiveStreams.load() == 3);

        metrics.recordRequestSuccess(start1, 10, 1, 100, 100, false);
        metrics.recordRequestSuccess(start2, 10, 1, 100, 100, false);

        // Peak should still be 3
        REQUIRE(metrics.gauges.peakActiveStreams.load() == 3);

        metrics.recordRequestSuccess(start3, 10, 1, 100, 100, false);
    }

    SECTION("Memory usage is tracked") {
        auto& metrics = ArrowMetrics::instance();

        metrics.updateMemoryUsage(1024);
        REQUIRE(metrics.gauges.currentMemoryUsage.load() == 1024);
        REQUIRE(metrics.gauges.peakMemoryUsage.load() == 1024);

        metrics.updateMemoryUsage(2048);
        REQUIRE(metrics.gauges.currentMemoryUsage.load() == 3072);
        REQUIRE(metrics.gauges.peakMemoryUsage.load() == 3072);

        metrics.updateMemoryUsage(-1024);
        REQUIRE(metrics.gauges.currentMemoryUsage.load() == 2048);
        // Peak should still be 3072
        REQUIRE(metrics.gauges.peakMemoryUsage.load() == 3072);
    }
}

TEST_CASE("Arrow Metrics Histograms", "[arrow][metrics][histograms]") {
    ArrowMetrics::instance().reset();

    SECTION("Duration histogram tracks min/max") {
        auto& metrics = ArrowMetrics::instance();

        // First request - will set initial values
        auto start1 = metrics.recordRequestStart();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        metrics.recordRequestSuccess(start1, 100, 1, 1000, 1000, false);

        uint64_t minDuration = metrics.histograms.minDurationUs.load();
        uint64_t maxDuration = metrics.histograms.maxDurationUs.load();

        // Should be at least 10ms = 10000us
        REQUIRE(minDuration >= 1000);  // At least 1ms (allowing for timing variance)
        REQUIRE(maxDuration >= minDuration);
    }

    SECTION("Batch size histogram tracks min/max") {
        auto& metrics = ArrowMetrics::instance();

        metrics.recordBatchStats(100);
        REQUIRE(metrics.histograms.minBatchRows.load() == 100);
        REQUIRE(metrics.histograms.maxBatchRows.load() == 100);

        metrics.recordBatchStats(50);
        REQUIRE(metrics.histograms.minBatchRows.load() == 50);
        REQUIRE(metrics.histograms.maxBatchRows.load() == 100);

        metrics.recordBatchStats(200);
        REQUIRE(metrics.histograms.minBatchRows.load() == 50);
        REQUIRE(metrics.histograms.maxBatchRows.load() == 200);
    }

    SECTION("Response size histogram tracks min/max") {
        auto& metrics = ArrowMetrics::instance();

        auto start1 = metrics.recordRequestStart();
        metrics.recordRequestSuccess(start1, 100, 1, 1024, 1024, false);

        REQUIRE(metrics.histograms.minResponseBytes.load() == 1024);
        REQUIRE(metrics.histograms.maxResponseBytes.load() == 1024);

        auto start2 = metrics.recordRequestStart();
        metrics.recordRequestSuccess(start2, 100, 1, 512, 512, false);

        REQUIRE(metrics.histograms.minResponseBytes.load() == 512);
        REQUIRE(metrics.histograms.maxResponseBytes.load() == 1024);
    }

    SECTION("Compression ratio is tracked") {
        auto& metrics = ArrowMetrics::instance();

        auto start1 = metrics.recordRequestStart();
        // 50% compression: 1000 bytes -> 500 bytes
        metrics.recordRequestSuccess(start1, 100, 1, 1000, 500, true);

        REQUIRE(metrics.histograms.compressionRatioCount.load() == 1);

        // Check average compression ratio (0.5 = 50%)
        double avgRatio = metrics.getAverageCompressionRatio();
        REQUIRE_THAT(avgRatio, Catch::Matchers::WithinAbs(0.5, 0.01));
    }
}

TEST_CASE("Arrow Metrics Request Scope", "[arrow][metrics][scope]") {
    ArrowMetrics::instance().reset();

    SECTION("Request scope tracks lifecycle correctly") {
        auto& metrics = ArrowMetrics::instance();

        {
            ArrowRequestScope scope;
            REQUIRE(metrics.gauges.activeStreams.load() == 1);
            scope.recordSuccess(100, 1, 1000, 800, true);
        }

        REQUIRE(metrics.gauges.activeStreams.load() == 0);
        REQUIRE(metrics.counters.successfulRequests.load() == 1);
    }

    SECTION("Abandoned request is recorded as failure") {
        auto& metrics = ArrowMetrics::instance();

        {
            ArrowRequestScope scope;
            REQUIRE(metrics.gauges.activeStreams.load() == 1);
            // Destructor called without recording success or failure
        }

        REQUIRE(metrics.gauges.activeStreams.load() == 0);
        REQUIRE(metrics.counters.failedRequests.load() == 1);
    }

    SECTION("Multiple scopes track independently") {
        auto& metrics = ArrowMetrics::instance();

        ArrowRequestScope scope1;
        REQUIRE(metrics.gauges.activeStreams.load() == 1);

        {
            ArrowRequestScope scope2;
            REQUIRE(metrics.gauges.activeStreams.load() == 2);
            scope2.recordSuccess(50, 1, 500, 500, false);
        }

        REQUIRE(metrics.gauges.activeStreams.load() == 1);
        scope1.recordSuccess(100, 2, 1000, 800, true);

        REQUIRE(metrics.gauges.activeStreams.load() == 0);
        REQUIRE(metrics.counters.successfulRequests.load() == 2);
    }
}

TEST_CASE("Arrow Metrics Thread Safety", "[arrow][metrics][threading]") {
    ArrowMetrics::instance().reset();

    SECTION("Concurrent requests are counted correctly") {
        auto& metrics = ArrowMetrics::instance();
        constexpr int numThreads = 10;
        constexpr int requestsPerThread = 100;

        std::vector<std::thread> threads;
        threads.reserve(numThreads);

        for (int t = 0; t < numThreads; t++) {
            threads.emplace_back([&metrics]() {
                for (int i = 0; i < requestsPerThread; i++) {
                    auto start = metrics.recordRequestStart();
                    metrics.recordRequestSuccess(start, 10, 1, 100, 80, true);
                }
            });
        }

        for (auto& thread : threads) {
            thread.join();
        }

        REQUIRE(metrics.counters.totalRequests.load() == numThreads * requestsPerThread);
        REQUIRE(metrics.counters.successfulRequests.load() == numThreads * requestsPerThread);
        REQUIRE(metrics.counters.totalRows.load() == numThreads * requestsPerThread * 10);
        REQUIRE(metrics.gauges.activeStreams.load() == 0);
    }
}

TEST_CASE("Arrow Metrics Calculations", "[arrow][metrics][calculations]") {
    ArrowMetrics::instance().reset();

    SECTION("Average duration calculation") {
        auto& metrics = ArrowMetrics::instance();

        // Simulate requests (can't control exact timing)
        auto start1 = metrics.recordRequestStart();
        metrics.recordRequestSuccess(start1, 100, 1, 1000, 1000, false);

        auto start2 = metrics.recordRequestStart();
        metrics.recordRequestSuccess(start2, 100, 1, 1000, 1000, false);

        double avgDuration = metrics.getAverageDurationUs();
        REQUIRE(avgDuration >= 0);  // Just verify it's computed
    }

    SECTION("Average compression ratio calculation") {
        auto& metrics = ArrowMetrics::instance();

        // 50% compression
        auto start1 = metrics.recordRequestStart();
        metrics.recordRequestSuccess(start1, 100, 1, 1000, 500, true);

        // 25% compression
        auto start2 = metrics.recordRequestStart();
        metrics.recordRequestSuccess(start2, 100, 1, 1000, 250, true);

        // Average should be ~37.5%
        double avgRatio = metrics.getAverageCompressionRatio();
        REQUIRE_THAT(avgRatio, Catch::Matchers::WithinAbs(0.375, 0.01));
    }

    SECTION("No compression requests returns 1.0 ratio") {
        auto& metrics = ArrowMetrics::instance();

        double avgRatio = metrics.getAverageCompressionRatio();
        REQUIRE_THAT(avgRatio, Catch::Matchers::WithinAbs(1.0, 0.001));
    }
}

TEST_CASE("Arrow Metrics Reset", "[arrow][metrics][reset]") {
    SECTION("Reset clears all metrics") {
        auto& metrics = ArrowMetrics::instance();

        // Generate some data
        auto start = metrics.recordRequestStart();
        metrics.recordRequestSuccess(start, 100, 2, 1000, 500, true);
        metrics.recordBatchStats(100);
        metrics.updateMemoryUsage(2048);

        // Verify non-zero values
        REQUIRE(metrics.counters.totalRequests.load() > 0);
        REQUIRE(metrics.counters.successfulRequests.load() > 0);
        REQUIRE(metrics.counters.totalBatches.load() > 0);

        // Reset
        metrics.reset();

        // Verify all zeroed
        REQUIRE(metrics.counters.totalRequests.load() == 0);
        REQUIRE(metrics.counters.successfulRequests.load() == 0);
        REQUIRE(metrics.counters.failedRequests.load() == 0);
        REQUIRE(metrics.counters.totalBatches.load() == 0);
        REQUIRE(metrics.counters.totalRows.load() == 0);
        REQUIRE(metrics.gauges.activeStreams.load() == 0);
        REQUIRE(metrics.gauges.currentMemoryUsage.load() == 0);
        REQUIRE(metrics.gauges.peakActiveStreams.load() == 0);
        REQUIRE(metrics.histograms.maxDurationUs.load() == 0);
        REQUIRE(metrics.histograms.maxBatchRows.load() == 0);
    }
}
