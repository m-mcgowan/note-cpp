// Program-wide operator new/delete hooks backing tests/common/alloc_counter.hpp.
// Linked into every doctest binary that needs alloc-profile assertions.
//
// On host and on ESP-IDF, std::malloc is the backing store (ESP-IDF's newlib
// routes std::malloc → heap_caps_malloc, so the C++ hook still catches every
// note-cpp allocation). The counter is silent when g_alloc.enabled is false
// — one atomic-load branch per allocation, no observable cost to non-measurement
// tests.
//
// Each allocation stashes its size in a header prepended to the user pointer so
// delete can subtract the correct amount from bytes_live. Alignment respects
// std::max_align_t to stay correct for the common case; we do NOT implement
// over-aligned new (C++17 aligned-new would need a separate header strategy).

#include "common/alloc_counter.hpp"

#include <cstdlib>
#include <new>

namespace note_tests {
AllocCounter g_alloc;
}  // namespace note_tests

namespace {

struct Header {
    std::size_t size;
};

constexpr std::size_t kHeaderBytes =
    (sizeof(Header) + alignof(std::max_align_t) - 1) &
    ~(alignof(std::max_align_t) - 1);

void* tracked_alloc(std::size_t n) {
    void* raw = std::malloc(kHeaderBytes + n);
    if (!raw) throw std::bad_alloc{};
    auto* hdr = static_cast<Header*>(raw);
    hdr->size = n;
    void* user = static_cast<char*>(raw) + kHeaderBytes;
    auto& a = note_tests::g_alloc;
    if (a.enabled.load(std::memory_order_acquire)) {
        a.count.fetch_add(1, std::memory_order_relaxed);
        auto live = a.bytes_live.fetch_add(n, std::memory_order_relaxed) + n;
        a.bytes_total.fetch_add(n, std::memory_order_relaxed);
        // Best-effort peak update. Races are tolerable — peak is diagnostic.
        auto peak = a.bytes_peak.load(std::memory_order_relaxed);
        while (live > peak &&
               !a.bytes_peak.compare_exchange_weak(peak, live,
                                                   std::memory_order_relaxed)) {
        }
    }
    return user;
}

void tracked_free(void* user) noexcept {
    if (!user) return;
    void* raw = static_cast<char*>(user) - kHeaderBytes;
    auto* hdr = static_cast<Header*>(raw);
    std::size_t n = hdr->size;
    auto& a = note_tests::g_alloc;
    if (a.enabled.load(std::memory_order_acquire)) {
        a.frees.fetch_add(1, std::memory_order_relaxed);
        // Saturating subtract — if a buffer leaked past a previous scope the
        // subtraction would underflow; clamp.
        auto cur = a.bytes_live.load(std::memory_order_relaxed);
        while (cur > 0 && !a.bytes_live.compare_exchange_weak(
                              cur, cur >= n ? cur - n : 0,
                              std::memory_order_relaxed)) {
        }
    }
    std::free(raw);
}

}  // namespace

void* operator new(std::size_t n)                        { return tracked_alloc(n); }
void* operator new[](std::size_t n)                      { return tracked_alloc(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    try { return tracked_alloc(n); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
    try { return tracked_alloc(n); } catch (...) { return nullptr; }
}

void operator delete(void* p) noexcept                     { tracked_free(p); }
void operator delete[](void* p) noexcept                   { tracked_free(p); }
void operator delete(void* p, std::size_t) noexcept        { tracked_free(p); }
void operator delete[](void* p, std::size_t) noexcept      { tracked_free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept   { tracked_free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { tracked_free(p); }
