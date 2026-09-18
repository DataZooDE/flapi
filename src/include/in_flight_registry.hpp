#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>

namespace flapi {

/**
 * How long the oldest request currently being served has been running.
 *
 * Exists because a backend can wedge without anything reporting it. A SQLite
 * attachment that loses its write transaction stops answering entirely, while
 * DuckDB, the HTTP server and every other connection carry on - so `SELECT 1`
 * readiness passes, the orchestrator sees a healthy instance, and traffic keeps
 * arriving at an endpoint that will never answer again (#116).
 *
 * Probing the wedged resource directly does not work: the probe blocks too, and
 * leaks a thread per health check. Measuring the SYMPTOM does - and it catches
 * any stall, not only this one.
 *
 * Cost on the hot path is one CAS to claim a slot and one store to release it,
 * with no allocation and no lock. Slots are handles rather than thread-locals
 * so a response completed on a different thread still releases correctly.
 */
class InFlightRegistry {
public:
    /// Opaque handle for an in-flight request. kNone when no slot was free.
    using Slot = std::size_t;
    static constexpr Slot kNone = static_cast<Slot>(-1);

    /// Claim a slot for a request starting at `now`. Returns kNone if the
    /// registry is full, in which case the request is simply untracked.
    [[nodiscard]] static Slot begin(std::chrono::steady_clock::time_point now) noexcept;

    /// Release a slot. Safe with kNone, and safe from a DIFFERENT thread than
    /// begin() - deliberately, because a response completed on another thread
    /// would otherwise latch its slot forever and wedge readiness with a
    /// permanent false stall.
    static void end(Slot slot) noexcept;

    /// Age of the oldest in-flight request, or zero when nothing is in flight.
    static std::chrono::milliseconds oldestAge(
        std::chrono::steady_clock::time_point now) noexcept;

    /// Number of requests currently in flight.
    static std::size_t inFlight() noexcept;

    /// Test seam: forget every slot. NOT safe while requests are running.
    static void resetForTesting() noexcept;

    // Bounded, so the scan in oldestAge() is O(threads) over a fixed array and
    // never allocates. Far above any realistic Crow pool; a thread beyond this
    // simply is not tracked rather than failing.
    static constexpr std::size_t kMaxSlots = 512;
};

}  // namespace flapi
