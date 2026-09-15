#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include "flapi_build_config.hpp"
#include "trace_context.hpp"
#include "trace_scope.hpp"
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

private:
    bool active() const;

    std::unique_ptr<ITracingBackend> backend_;
    bool enabled_ = false;      // BR-6: off until an operator turns it on
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
