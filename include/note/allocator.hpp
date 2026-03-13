// Pluggable allocator for note-cpp.
//
// A simple function-pointer allocator that works everywhere (no C++ allocator
// model, no std::pmr dependency). Adapters provided for MonotonicArena and
// std::pmr::memory_resource (when available).
//
// Usage:
//   // Default: heap allocation
//   note::Allocator alloc;
//
//   // Arena:
//   char pool[4096];
//   note::MonotonicArena arena(pool);
//   auto alloc = note::arena_allocator(arena);
//
//   // pmr (when available):
//   std::pmr::monotonic_buffer_resource res(buf, sizeof(buf));
//   auto alloc = note::pmr_allocator(&res);
#pragma once

#include <note/arena.hpp>

#include <cstddef>
#include <new>

namespace note {

struct Allocator {
    void* (*alloc)(size_t n, void* ctx) = default_alloc;
    void  (*free)(void* p, size_t n, void* ctx) = default_free;
    void* ctx = nullptr;

    void* allocate(size_t n) const { return alloc(n, ctx); }
    void deallocate(void* p, size_t n) const { free(p, n, ctx); }

    static void* default_alloc(size_t n, void*) { return ::operator new(n); }
    static void  default_free(void* p, size_t, void*) { ::operator delete(p); }
};

// Arena adapter — routes allocations through a MonotonicArena.
// Free is a no-op (arena reclaims on reset).
inline Allocator arena_allocator(MonotonicArena& a) {
    return {
        [](size_t n, void* ctx) -> void* {
            return static_cast<MonotonicArena*>(ctx)->allocate(n);
        },
        [](void*, size_t, void*) {},
        &a
    };
}

} // namespace note

// pmr adapter — routes through std::pmr::memory_resource (C++17).
#if __has_include(<memory_resource>)
#include <memory_resource>

namespace note {

inline Allocator pmr_allocator(std::pmr::memory_resource* r) {
    return {
        [](size_t n, void* ctx) -> void* {
            return static_cast<std::pmr::memory_resource*>(ctx)->allocate(
                n, alignof(std::max_align_t));
        },
        [](void* p, size_t n, void* ctx) {
            static_cast<std::pmr::memory_resource*>(ctx)->deallocate(
                p, n, alignof(std::max_align_t));
        },
        r
    };
}

} // namespace note
#endif
