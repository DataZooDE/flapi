#pragma once

// SpanScope::Impl, shared between trace_scope.cpp and flapi_tracing.cpp.
//
// Deliberately NOT in src/include/: it names OpenTelemetry types, and the whole
// point of the opaque handle is that no header under src/include/ drags protobuf
// and abseil into ~80 translation units and the test binary's link surface.
#include "trace_scope.hpp"

#if FLAPI_WITH_TRACING

#include <optional>

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
    // Optional so the activation can be released EARLY, before the span ends.
    //
    // OpenTelemetry's context stack detaches strictly LIFO. While a handler
    // ran inline that was automatic: push in before_handle, pop in finish(),
    // strictly nested on one thread. Once a handler can be offloaded (#120),
    // two requests on the same io thread can finish in the other order, and an
    // out-of-order Detach fails - leaving a token on the stack, so every later
    // request on that thread without its own traceparent parents to an ended
    // span, and each mismatched pair leaks a shared_ptr.
    //
    // The offload path therefore releases this immediately after handing the
    // work over, while it is still the top of the stack, and the worker
    // re-activates on its own thread. Inline paths are unchanged.
    std::optional<opentelemetry::trace::Scope> scope;

    explicit Impl(opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span> s)
        : span(s), scope(std::in_place, s) {}
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
