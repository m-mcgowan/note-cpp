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
// The counter is disabled by default; overhead in non-measurement paths is
// a single relaxed atomic load per allocation. Measurement is not nested —
// a new ScopedAllocCounter resets counts on entry and restores the previous
// enabled state on exit (nested scopes are not supported; don't nest them).

#include <atomic>
#include <cstddef>

namespace note_tests {

struct AllocCounter {
    std::atomic<std::size_t> count{0};
    std::atomic<std::size_t> frees{0};
    std::atomic<std::size_t> bytes_live{0};
    std::atomic<std::size_t> bytes_peak{0};
    std::atomic<std::size_t> bytes_total{0};
    std::atomic<bool> enabled{false};
};

extern AllocCounter g_alloc;

class ScopedAllocCounter {
public:
    ScopedAllocCounter() {
        // Reset counts; record that the scope is active.
        g_alloc.count.store(0, std::memory_order_relaxed);
        g_alloc.frees.store(0, std::memory_order_relaxed);
        g_alloc.bytes_live.store(0, std::memory_order_relaxed);
        g_alloc.bytes_peak.store(0, std::memory_order_relaxed);
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
    std::size_t bytes_live()  const { return g_alloc.bytes_live.load(std::memory_order_relaxed); }
    std::size_t bytes_peak()  const { return g_alloc.bytes_peak.load(std::memory_order_relaxed); }
    std::size_t bytes_total() const { return g_alloc.bytes_total.load(std::memory_order_relaxed); }
};

} // namespace note_tests
