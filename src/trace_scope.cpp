// The FLAPI_WITH_TRACING=ON implementation of SpanScope.
//
// One of only a handful of translation units permitted to include
// <opentelemetry/...>. Keeping those headers out of src/include/ is what stops
// protobuf and abseil reaching ~80 TUs and the test binary's link surface.
#include "trace_scope.hpp"
#include "trace_scope_internal.hpp"

#if FLAPI_WITH_TRACING

#include <opentelemetry/trace/provider.h>
#include <opentelemetry/trace/scope.h>
#include <opentelemetry/trace/span.h>

#include <utility>

namespace flapi {
inline namespace tracing_on_v1 {

namespace otel = opentelemetry;

SpanScope::~SpanScope() {
    end();
}

SpanScope::Activation::~Activation() {
    delete impl_;
}

SpanScope::Activation& SpanScope::Activation::operator=(Activation&& other) noexcept {
    if (this != &other) {
        delete impl_;
        impl_ = other.impl_;
        other.impl_ = nullptr;
    }
    return *this;
}

SpanScope::Activation SpanScope::activateOnThisThread() const noexcept {
    if (impl_ == nullptr) {
        return Activation{};
    }
    return Activation(new Activation::Impl(impl_->span));
}

SpanScope& SpanScope::operator=(SpanScope&& other) noexcept {
    if (this != &other) {
        end();
        impl_ = other.impl_;
        other.impl_ = nullptr;
    }
    return *this;
}

void SpanScope::setAttr(const char* key, std::string_view value) noexcept {
    if (impl_ == nullptr) { return; }
    // Copied into the recordable immediately. Retaining the view would be a
    // use-after-free whenever the caller passes a temporary - the classic
    // OpenTelemetry-C++ footgun.
    impl_->span->SetAttribute(key, otel::nostd::string_view(value.data(), value.size()));
}

void SpanScope::setAttr(const char* key, const char* value) noexcept {
    if (impl_ != nullptr && value != nullptr) {
        impl_->span->SetAttribute(key, otel::nostd::string_view(value));
    }
}

void SpanScope::setAttr(const char* key, std::int64_t value) noexcept {
    if (impl_ != nullptr) { impl_->span->SetAttribute(key, value); }
}

void SpanScope::setAttr(const char* key, double value) noexcept {
    if (impl_ != nullptr) { impl_->span->SetAttribute(key, value); }
}

void SpanScope::setAttr(const char* key, bool value) noexcept {
    if (impl_ != nullptr) { impl_->span->SetAttribute(key, value); }
}

void SpanScope::addEvent(const char* name) noexcept {
    if (impl_ != nullptr) { impl_->span->AddEvent(name); }
}

void SpanScope::addLink(const SpanContextIds&) noexcept {
    // Links must be supplied at span creation in the OTel C++ API; this exists so
    // call sites compile now and is wired up with the Tasks work (issue 15).
}

void SpanScope::setError(const char* error_type) noexcept {
    if (impl_ == nullptr) { return; }
    impl_->span->SetAttribute("error.type", error_type);
    // No description: a free-form message here is the most reliable way to leak
    // customer data into a trace.
    impl_->span->SetStatus(otel::trace::StatusCode::kError);
}

void SpanScope::end() noexcept {
    if (impl_ == nullptr) { return; }
    impl_->span->End();
    delete impl_;
    impl_ = nullptr;
}

void SpanScope::updateName(std::string_view name) noexcept {
    if (impl_ != nullptr) {
        impl_->span->UpdateName(otel::nostd::string_view(name.data(), name.size()));
    }
}

bool SpanScope::recording() const noexcept {
    if (impl_ == nullptr || !impl_->span) {
        return false;
    }
    try {
        return impl_->span->IsRecording();
    } catch (...) {
        return false;
    }
}

SpanContextIds SpanScope::ids() const noexcept {
    SpanContextIds out;
    if (impl_ == nullptr) { return out; }
    const auto ctx = impl_->span->GetContext();
    if (!ctx.IsValid()) { return out; }

    char trace_buf[32];
    char span_buf[16];
    ctx.trace_id().ToLowerBase16(trace_buf);
    ctx.span_id().ToLowerBase16(span_buf);
    out.trace_id.assign(trace_buf, sizeof(trace_buf));
    out.span_id.assign(span_buf, sizeof(span_buf));
    out.flags = ctx.trace_flags().flags();
    return out;
}

}  // inline namespace
}  // namespace flapi

#endif  // FLAPI_WITH_TRACING
