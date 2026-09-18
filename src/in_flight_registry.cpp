#include "in_flight_registry.hpp"

namespace flapi {

namespace {

// Nanoseconds since the steady clock epoch; 0 means the slot is idle. An
// integer rather than a time_point so it fits in a lock-free atomic on every
// platform we build for.
using SlotValue = std::int64_t;

std::array<std::atomic<SlotValue>, InFlightRegistry::kMaxSlots>& slots() {
    static std::array<std::atomic<SlotValue>, InFlightRegistry::kMaxSlots> storage{};
    return storage;
}

std::atomic<std::size_t>& hint() {
    static std::atomic<std::size_t> next{0};
    return next;
}

// Highest slot index ever claimed, so the scans below stay proportional to the
// real concurrency rather than always walking all kMaxSlots.
std::atomic<std::size_t>& highWaterMark() {
    static std::atomic<std::size_t> mark{0};
    return mark;
}

void highWater(std::size_t idx) {
    std::size_t current = highWaterMark().load(std::memory_order_relaxed);
    while (idx + 1 > current &&
           !highWaterMark().compare_exchange_weak(current, idx + 1,
                                                  std::memory_order_relaxed)) {
    }
}

SlotValue toNanos(std::chrono::steady_clock::time_point tp) noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               tp.time_since_epoch()).count();
}

}  // namespace

InFlightRegistry::Slot InFlightRegistry::begin(
    std::chrono::steady_clock::time_point now) noexcept {
    // A zero timestamp would read as idle. Vanishingly unlikely, but ruling it
    // out costs one comparison on a path that must never misreport.
    SlotValue value = toNanos(now);
    if (value == 0) {
        value = 1;
    }

    // Claim the first free slot, starting from a rotating hint so concurrent
    // claims do not all contend on index 0. Bounded by kMaxSlots, so this never
    // spins: past the cap the request is untracked, which under-reports rather
    // than blocking a request path.
    const std::size_t start = hint().fetch_add(1, std::memory_order_relaxed);
    for (std::size_t i = 0; i < kMaxSlots; ++i) {
        const std::size_t idx = (start + i) % kMaxSlots;
        SlotValue expected = 0;
        if (slots()[idx].compare_exchange_strong(expected, value,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_relaxed)) {
            highWater(idx);
            return idx;
        }
    }
    return kNone;
}

void InFlightRegistry::end(Slot slot) noexcept {
    if (slot == kNone || slot >= kMaxSlots) {
        return;
    }
    slots()[slot].store(0, std::memory_order_release);
}

std::chrono::milliseconds InFlightRegistry::oldestAge(
    std::chrono::steady_clock::time_point now) noexcept {
    const SlotValue cutoff = toNanos(now);
    SlotValue oldest = 0;
    const std::size_t used = highWaterMark().load(std::memory_order_acquire);
    const std::size_t limit = used < kMaxSlots ? used : kMaxSlots;

    for (std::size_t i = 0; i < limit; ++i) {
        const SlotValue started = slots()[i].load(std::memory_order_relaxed);
        if (started == 0) {
            continue;   // idle
        }
        if (oldest == 0 || started < oldest) {
            oldest = started;
        }
    }
    if (oldest == 0 || cutoff <= oldest) {
        return std::chrono::milliseconds(0);
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::nanoseconds(cutoff - oldest));
}

std::size_t InFlightRegistry::inFlight() noexcept {
    std::size_t count = 0;
    const std::size_t used = highWaterMark().load(std::memory_order_acquire);
    const std::size_t limit = used < kMaxSlots ? used : kMaxSlots;
    for (std::size_t i = 0; i < limit; ++i) {
        if (slots()[i].load(std::memory_order_relaxed) != 0) {
            ++count;
        }
    }
    return count;
}

void InFlightRegistry::resetForTesting() noexcept {
    for (std::size_t i = 0; i < kMaxSlots; ++i) {
        slots()[i].store(0, std::memory_order_relaxed);
    }
    highWaterMark().store(0, std::memory_order_relaxed);
}

}  // namespace flapi
