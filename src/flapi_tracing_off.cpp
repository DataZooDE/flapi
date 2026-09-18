// The FLAPI_WITH_TRACING=OFF twin of the tracing facade.
//
// Keeps every call site compiling with no #ifdef of its own, and guarantees the
// OFF build constructs no provider and starts no exporter thread.
#include "flapi_tracing.hpp"

#if !FLAPI_WITH_TRACING

namespace flapi {

FlapiTracing::FlapiTracing() = default;
FlapiTracing::FlapiTracing(std::unique_ptr<ITracingBackend> backend)
    : backend_(std::move(backend)) {}
FlapiTracing::~FlapiTracing() = default;

void FlapiTracing::configure(const TracingConfig&) {
    // Deliberately nothing: with tracing compiled out, configuration cannot turn
    // it on. An operator who set tracing.enabled here gets the startup warning
    // emitted by main(), not a silent no-op.
}

bool FlapiTracing::active() const { return false; }
bool FlapiTracing::isEnabled() const { return false; }

SpanScope FlapiTracing::startSpan(const char*, SpanKind, const SpanContextIds*) { return {}; }
SpanScope FlapiTracing::startServerSpan(const std::string&, const ExtractedContext&) { return {}; }

bool FlapiTracing::forceFlush(std::chrono::milliseconds) { return true; }
void FlapiTracing::shutdown() {}
std::uint64_t FlapiTracing::spansDropped() const { return 0; }
std::uint64_t FlapiTracing::spansExported() const { return 0; }

FlapiTracing& Tracing() {
    static FlapiTracing instance;
    return instance;
}

TracingGuard::~TracingGuard() = default;

}  // namespace flapi

#endif  // !FLAPI_WITH_TRACING
