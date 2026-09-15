#include "alloc_counter.hpp"

#include <cstdlib>
#include <new>

namespace flapi::test {

std::atomic<std::size_t>& allocCount() {
    static std::atomic<std::size_t> count{0};
    return count;
}

std::atomic<bool>& allocCounting() {
    static std::atomic<bool> counting{false};
    return counting;
}

}  // namespace flapi::test

// Replacing global operator new is the only portable way to count allocations
// without a sanitizer or an allocator shim. Exactly one definition, here.
void* operator new(std::size_t size) {
    if (flapi::test::allocCounting().load(std::memory_order_relaxed)) {
        flapi::test::allocCount().fetch_add(1, std::memory_order_relaxed);
    }
    void* p = std::malloc(size ? size : 1);
    if (!p) { throw std::bad_alloc(); }
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
