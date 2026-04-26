// Program-wide operator new/delete hooks backing tests/common/alloc_counter.hpp.
// Linked into every doctest binary that needs alloc-profile assertions.
//
// Pure passthrough to std::malloc / std::free. When g_alloc.enabled is true we
// bump call/byte counters; otherwise the operator-new/delete pair behaves
// indistinguishably from the default. No malloc header is prepended — that
// scheme aborted on ESP32 when static-init OOM raised bad_alloc into a runtime
// that wasn't ready to unwind.

#include "common/alloc_counter.hpp"

#include <cstdlib>
#include <new>

namespace note_tests {
AllocCounter g_alloc;
InitCounter  g_init;
}  // namespace note_tests

namespace {

inline void* tracked_alloc(std::size_t n) noexcept {
    void* p = std::malloc(n);
    if (!p) std::abort();  // OOM — can't recover during static init; abort cleanly.
    auto& a = note_tests::g_alloc;
    if (a.enabled.load(std::memory_order_acquire)) {
        a.count.fetch_add(1, std::memory_order_relaxed);
        a.bytes_total.fetch_add(n, std::memory_order_relaxed);
    }
    auto& init = note_tests::g_init;
    if (!init.frozen.load(std::memory_order_acquire)) {
        init.count.fetch_add(1, std::memory_order_relaxed);
        init.bytes.fetch_add(n, std::memory_order_relaxed);
    }
    return p;
}

inline void tracked_free(void* p) noexcept {
    if (!p) return;
    auto& a = note_tests::g_alloc;
    if (a.enabled.load(std::memory_order_acquire)) {
        a.frees.fetch_add(1, std::memory_order_relaxed);
    }
    std::free(p);
}

}  // namespace

void* operator new(std::size_t n)                        { return tracked_alloc(n); }
void* operator new[](std::size_t n)                      { return tracked_alloc(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return tracked_alloc(n); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return tracked_alloc(n); }

void operator delete(void* p) noexcept                     { tracked_free(p); }
void operator delete[](void* p) noexcept                   { tracked_free(p); }
void operator delete(void* p, std::size_t) noexcept        { tracked_free(p); }
void operator delete[](void* p, std::size_t) noexcept      { tracked_free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept   { tracked_free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { tracked_free(p); }
