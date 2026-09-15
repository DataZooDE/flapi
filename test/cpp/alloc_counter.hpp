#pragma once

// A process-wide allocation counter, used by the NFR-1 proxy gates.
//
// The global operator new replacement lives in exactly ONE translation unit
// (alloc_counter.cpp) - two definitions are a duplicate-symbol link error - so
// any test that needs allocation counts includes this header instead of writing
// its own.
//
// Not thread-safe against other allocating tests running concurrently; Catch2
// runs single-threaded by default, which is what makes this sound.
#include <atomic>
#include <cstddef>

namespace flapi::test {

std::atomic<std::size_t>& allocCount();
std::atomic<bool>& allocCounting();

// Counts allocations for its lifetime.
struct AllocCounter {
    AllocCounter() { allocCount().store(0); allocCounting().store(true); }
    ~AllocCounter() { allocCounting().store(false); }
    std::size_t count() const { return allocCount().load(); }
};

// C++14 lets the compiler elide an unused new/delete pair, and at -O3 it does.
// Every allocation under test must pass through a barrier or the gate measures
// nothing at all.
template <typename T>
inline void doNotOptimise(T& value) {
    asm volatile("" : "+m"(value) : : "memory");
}

}  // namespace flapi::test
