#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include "flapi_build_config.hpp"
#include "trace_context.hpp"
#include "trace_scope.hpp"
#include <optional>
#include "tracing_config.hpp"

namespace flapi {

// Lifecycle facade for tracing.
//
// Deliberately isomorphic to FlapiTelemetry (facade + injectable backend + a
// single active() gate) so the two observability subsystems read alike, with one
// deliberate difference: Tracing() is NOT a leaked singleton. FlapiTelemetry can
// be leaked safely because it owns no thread; a TracerProvider owns a
// BatchSpanProcessor export thread, and letting static destruction race it is how
// you get an intermittent crash at exit. A TracingGuard in main() shuts it down
// deterministically instead.
struct ITracingBackend {
    virtual ~ITracingBackend() = default;
    virtual SpanScope startSpan(const char* name, SpanKind kind, const SpanContextIds* parent) = 0;
    virtual bool forceFlush(std::chrono::milliseconds timeout) = 0;
    virtual void shutdown() = 0;
    virtual std::uint64_t spansDropped() const = 0;
    virtual std::uint64_t spansExported() const = 0;
};

class FlapiTracing {
public:
    FlapiTracing();
    explicit FlapiTracing(std::unique_ptr<ITracingBackend> backend);
    ~FlapiTracing();

    FlapiTracing(const FlapiTracing&) = delete;
    FlapiTracing& operator=(const FlapiTracing&) = delete;

    // Builds the provider, sampler and exporter. Safe to call with a disabled
    // config: nothing is constructed and no thread starts (NFR-1).
    void configure(const TracingConfig& config);

    bool isEnabled() const;

    SpanScope startSpan(const char* name, SpanKind kind, const SpanContextIds* parent = nullptr);
    SpanScope startServerSpan(const std::string& name, const ExtractedContext& parent);

    bool forceFlush(std::chrono::milliseconds timeout);
    void shutdown();

    std::uint64_t spansDropped() const;
    std::uint64_t spansExported() const;
    // Spans handed to the processor. The SDK drops silently on a full queue, so
    // submitted - exported - dropped is the only honest view of loss.
    std::uint64_t spansSubmitted() const;

    // How much DuckDB execution profiling the operator asked for. Read on the
    // query path, so it is a plain value read - no lock, no allocation. Stays
    // Off unless tracing is actually active, so a disabled build or a disabled
    // config can never make the query path pay for profiling.
    DbProfiling dbProfiling() const { return db_profiling_; }

    // Set only when an operator opted a NETWORK exporter into per-request
    // export with an explicit budget. Empty otherwise, which is the common case
    // and costs the request path a single pointer-sized check.
    std::optional<std::chrono::milliseconds> blockingFlush() const { return blocking_flush_; }

    // Budget for the shutdown force-flush. Was hardcoded at 2s, which meant an
    // operator who tuned flush.timeout_ms had no effect on the path that
    // matters most on a platform with a short SIGTERM grace.
    std::chrono::milliseconds shutdownFlushBudget() const { return shutdown_flush_; }

    // Requests whose span export did not finish inside the budget. Surfaced by
    // GET /api/v1/_config/metrics: a rising value means the collector is costing
    // callers latency AND still losing spans, which is the worst of both.
    std::uint64_t flushTimeouts() const;
    static void noteFlushTimeout();

private:
    bool active() const;

    std::unique_ptr<ITracingBackend> backend_;
    bool enabled_ = false;      // BR-6: off until an operator turns it on
    DbProfiling db_profiling_ = DbProfiling::Off;
    std::optional<std::chrono::milliseconds> blocking_flush_;
    std::chrono::milliseconds shutdown_flush_{2000};
};

// Process-wide accessor. Not leaked; see the note above.
FlapiTracing& Tracing();

// Deterministic shutdown of the tracer provider. Construct once in main().
class TracingGuard {
public:
    TracingGuard() = default;
    ~TracingGuard();
    TracingGuard(const TracingGuard&) = delete;
    TracingGuard& operator=(const TracingGuard&) = delete;
};

}  // namespace flapi
