// The FLAPI_WITH_TRACING=OFF twin of SpanScope.
//
// Compiled INSTEAD of trace_scope.cpp when tracing is disabled. Every operation
// is a no-op on a null handle, so call sites compile unchanged and the optimiser
// deletes the guarded blocks entirely.
//
// This file exists rather than an all-inline twin in the header because the
// versioned-namespace guard needs a real out-of-line symbol to reference; an
// inline-only twin emits nothing and a mismatched build would link silently.
#include "trace_scope.hpp"

#if !FLAPI_WITH_TRACING

namespace flapi {
inline namespace tracing_off_v1 {

SpanScope::~SpanScope() = default;

SpanScope::Activation::~Activation() = default;

SpanScope::Activation& SpanScope::Activation::operator=(Activation&& other) noexcept {
    impl_ = other.impl_;
    other.impl_ = nullptr;
    return *this;
}

void SpanScope::suspendActivation() noexcept {}

SpanScope::Activation SpanScope::activateOnThisThread() const noexcept {
    return Activation{};
}

SpanScope& SpanScope::operator=(SpanScope&& other) noexcept {
    impl_ = other.impl_;
    other.impl_ = nullptr;
    return *this;
}

void SpanScope::setAttr(const char*, std::string_view) noexcept {}
void SpanScope::setAttr(const char*, const char*) noexcept {}
void SpanScope::setAttr(const char*, std::int64_t) noexcept {}
void SpanScope::setAttr(const char*, double) noexcept {}
void SpanScope::setAttr(const char*, bool) noexcept {}
void SpanScope::addEvent(const char*) noexcept {}
void SpanScope::addLink(const SpanContextIds&) noexcept {}
void SpanScope::setError(const char*) noexcept {}
void SpanScope::end() noexcept {}
void SpanScope::updateName(std::string_view) noexcept {}

bool SpanScope::recording() const noexcept { return false; }

SpanContextIds SpanScope::ids() const noexcept { return {}; }

}  // inline namespace
}  // namespace flapi

#endif  // !FLAPI_WITH_TRACING
