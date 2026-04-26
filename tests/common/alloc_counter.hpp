#pragma once

// Shared allocation counter for tests that need to assert no-alloc behaviour.
//
// Wire the .cpp into any test binary that needs the counter. Files that just
// want to read counts include only this header.
//
// Usage:
//
//     #include "common/alloc_counter.hpp"
//
//     TEST_CASE("no heap allocations after warm-up") {
//         warm_up();
//         {
//             note_tests::ScopedAllocCounter scope;
//             do_hot_path();
//             CHECK(scope.count() == 0);
//         }
//     }
//
// Counters are zero outside an active ScopedAllocCounter; ctor resets them and
// flips `enabled` true, dtor flips it back. Nesting is not supported. Overhead
// outside a scope is one relaxed atomic load per allocation. The counter
// tracks call counts and a running total of bytes requested; it does NOT track
// per-allocation `bytes_live` (would require a malloc header which is unsafe
// during early static-init OOM on ESP32).

#include <atomic>
#include <cstddef>

namespace note_tests {

struct AllocCounter {
    std::atomic<std::size_t> count{0};
    std::atomic<std::size_t> frees{0};
    std::atomic<std::size_t> bytes_total{0};
    std::atomic<bool> enabled{false};
};

extern AllocCounter g_alloc;

// Always-on diagnostic counters covering everything from program start until
// `init_phase_freeze()` is called. Use to size up doctest-registration
// overhead and similar static-init heap usage. Header-only so the counter
// works in builds (e.g. ci.sh's quick subset) that don't compile
// alloc_counter.cpp.
struct InitCounter {
    std::atomic<std::size_t> count{0};
    std::atomic<std::size_t> bytes{0};
    std::atomic<bool> frozen{false};
};

inline InitCounter g_init;

inline void init_phase_freeze() { g_init.frozen.store(true, std::memory_order_release); }

class ScopedAllocCounter {
public:
    ScopedAllocCounter() {
        g_alloc.count.store(0, std::memory_order_relaxed);
        g_alloc.frees.store(0, std::memory_order_relaxed);
        g_alloc.bytes_total.store(0, std::memory_order_relaxed);
        g_alloc.enabled.store(true, std::memory_order_release);
    }
    ~ScopedAllocCounter() {
        g_alloc.enabled.store(false, std::memory_order_release);
    }

    ScopedAllocCounter(const ScopedAllocCounter&) = delete;
    ScopedAllocCounter& operator=(const ScopedAllocCounter&) = delete;

    std::size_t count()       const { return g_alloc.count.load(std::memory_order_relaxed); }
    std::size_t frees()       const { return g_alloc.frees.load(std::memory_order_relaxed); }
    std::size_t bytes_total() const { return g_alloc.bytes_total.load(std::memory_order_relaxed); }
};

} // namespace note_tests
