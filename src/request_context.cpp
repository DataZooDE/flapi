#include "request_context.hpp"

#include <random>

namespace flapi {

namespace {

// thread_local, so no synchronisation and no contention on the hot path. Crow's
// workers are pooled, which is exactly why RequestContextScope must clear it on
// every exit path rather than relying on the next request to overwrite it.
thread_local RequestContext* t_current = nullptr;

}  // namespace

void RequestContext::mintRequestId(std::array<char, 20>& out) {
    // Matches the format AuditLogger has always produced ("req-" + 16 hex), so
    // existing consumers of the audit log keep working. Server-minted always: an
    // inbound X-Request-Id is never trusted, or a client could forge collisions
    // and inject into log lines.
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    static constexpr char kHex[] = "0123456789abcdef";

    out[0] = 'r'; out[1] = 'e'; out[2] = 'q'; out[3] = '-';
    std::uint64_t v = rng();
    for (int i = 0; i < 16; ++i) {
        out[4 + i] = kHex[(v >> ((15 - i) * 4)) & 0xF];
    }
}

RequestContextScope::RequestContextScope(RequestContext* rc) noexcept
    : previous_(t_current) {
    t_current = rc;
}

RequestContextScope::~RequestContextScope() noexcept {
    t_current = previous_;
}

RequestContext* RequestContextScope::current() noexcept {
    return t_current;
}

void RequestContextScope::activate(RequestContext* rc) noexcept {
    t_current = rc;
}

void RequestContextScope::clear() noexcept {
    t_current = nullptr;
}

void RequestContextScope::clearIf(const RequestContext* expected) noexcept {
    if (t_current == expected) {
        t_current = nullptr;
    }
}

}  // namespace flapi
