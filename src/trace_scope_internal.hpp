#pragma once

// SpanScope::Impl, shared between trace_scope.cpp and flapi_tracing.cpp.
//
// Deliberately NOT in src/include/: it names OpenTelemetry types, and the whole
// point of the opaque handle is that no header under src/include/ drags protobuf
// and abseil into ~80 translation units and the test binary's link surface.
#include "trace_scope.hpp"

#if FLAPI_WITH_TRACING

#include <opentelemetry/trace/scope.h>
#include <opentelemetry/trace/span.h>

namespace flapi {
inline namespace tracing_on_v1 {

// The span plus its activation scope. Holding the Scope is what makes inner spans
// parent themselves automatically: OpenTelemetry keeps a thread-local active-span
// stack, so a child started anywhere down the call chain attaches to whatever is
// on top - which is why flAPI's inner pipeline needs no new parameters threaded
// through DatabaseManager, SQLTemplateProcessor and QueryExecutor.
struct SpanScope::Impl {
    opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span> span;
    opentelemetry::trace::Scope scope;

    explicit Impl(opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span> s)
        : span(s), scope(s) {}
};

// A second activation of an already-started span, for a different thread.
// Only the Scope: the span itself is owned by the SpanScope this came from,
// and must not be ended twice.
struct SpanScope::Activation::Impl {
    opentelemetry::trace::Scope scope;
    explicit Impl(opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span> s)
        : scope(s) {}
};

}  // inline namespace
}  // namespace flapi

#endif  // FLAPI_WITH_TRACING
