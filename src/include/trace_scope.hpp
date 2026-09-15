#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "flapi_build_config.hpp"
#include "trace_context.hpp"

namespace flapi {

enum class SpanKind { Internal, Server, Client, Producer, Consumer };

#if FLAPI_WITH_TRACING
inline namespace tracing_on_v1 {
#else
inline namespace tracing_off_v1 {
#endif

// An RAII handle to one span. Opaque and pointer-sized, so no OpenTelemetry type
// appears in any header under src/include/ - which keeps protobuf and abseil out
// of ~80 translation units and out of the test binary's link surface.
//
// Inert by default: a disabled span holds a null pointer, allocates nothing, and
// every method is a predicted branch. Guard BEFORE building an attribute value:
//
//     SpanScope s = Tracing().startSpan("flapi.render", SpanKind::Internal);
//     if (s) s.setAttr(kTemplatePath, configRelative(endpoint.templateSource));
//
// so the expensive argument is never computed when tracing is off.
//
// Strict RAII: the destructor ends the span. Without that, a BadRequestError
// thrown between startSpan and end() would leak the scope push and mis-parent the
// NEXT request on that pooled worker.
class SpanScope {
public:
    SpanScope() noexcept = default;
    ~SpanScope();

    SpanScope(SpanScope&& other) noexcept : impl_(other.impl_) { other.impl_ = nullptr; }
    SpanScope& operator=(SpanScope&& other) noexcept;
    SpanScope(const SpanScope&) = delete;
    SpanScope& operator=(const SpanScope&) = delete;

    explicit operator bool() const noexcept { return impl_ != nullptr; }

    void setAttr(const char* key, std::string_view value) noexcept;
    void setAttr(const char* key, std::int64_t value) noexcept;
    void setAttr(const char* key, double value) noexcept;
    void setAttr(const char* key, bool value) noexcept;
    void addEvent(const char* name) noexcept;
    void addLink(const SpanContextIds& linked) noexcept;
    // Enumerated values only. A free-form exception message must never become an
    // attribute: it is the most reliable way to leak customer data into a trace.
    void setError(const char* error_type) noexcept;
    void end() noexcept;

    SpanContextIds ids() const noexcept;

    struct Impl;
    explicit SpanScope(Impl* impl) noexcept : impl_(impl) {}

private:
    Impl* impl_ = nullptr;
};

// The mixed-build guard is SpanScope's own out-of-line methods: they mangle as
// tracing_on_v1::... or tracing_off_v1::..., so a TU compiled with a different
// FLAPI_WITH_TRACING fails to LINK rather than silently disagreeing about the
// layout. That is why these methods are deliberately NOT inline - an all-inline
// twin emits no symbols and the guard never fires.
// Verified by scripts/check_tracing_abi_guard.sh.

}  // inline namespace

}  // namespace flapi
