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
#include <cstdlib>
#include <cstring>
#include <new>

namespace note {

struct Allocator {
    void* (*alloc)(size_t n, void* ctx) = default_alloc;
    void  (*free)(void* p, size_t n, void* ctx) = default_free;
    void* (*realloc)(void* p, size_t old_n, size_t new_n, void* ctx) = default_realloc;
    void* ctx = nullptr;

    void* allocate(size_t n) const { return alloc(n, ctx); }
    void deallocate(void* p, size_t n) const { free(p, n, ctx); }
    void* reallocate(void* p, size_t old_n, size_t new_n) const { return realloc(p, old_n, new_n, ctx); }

    static void* default_alloc(size_t n, void*) { return std::malloc(n); }
    static void  default_free(void* p, size_t, void*) { std::free(p); }
    static void* default_realloc(void* p, size_t, size_t new_n, void*) {
        return std::realloc(p, new_n);  // may extend in place
    }
};

// Arena adapter — routes allocations through a MonotonicArena.
// Free is a no-op (arena reclaims on reset).
inline Allocator arena_allocator(MonotonicArena& a) {
    return {
        [](size_t n, void* ctx) -> void* {
            return static_cast<MonotonicArena*>(ctx)->allocate(n);
        },
        [](void*, size_t, void*) {},  // arena free is a no-op
        [](void* p, size_t old_n, size_t new_n, void* ctx) -> void* {
            // Arena can't extend in place — allocate new, copy, old is leaked
            // (reclaimed on arena reset).
            auto* arena = static_cast<MonotonicArena*>(ctx);
            void* np = arena->allocate(new_n);
            if (np && p) std::memcpy(np, p, old_n < new_n ? old_n : new_n);
            return np;
        },
        &a
    };
}

#include <note/note_config.hpp>
#if NOTE_NO_STD_STRING == 0
// HeapResetPool adapter — same shape as arena_allocator, malloc-backed.
// Free is a no-op (pool reclaims on reset). Use when you want the arena
// lifecycle (allocate fast, free all at once) but don't want to size a
// buffer up front.
inline Allocator heap_reset_allocator(HeapResetPool& p) {
    return {
        [](size_t n, void* ctx) -> void* {
            return static_cast<HeapResetPool*>(ctx)->allocate(n);
        },
        [](void*, size_t, void*) {},  // pool free is a no-op; reset drains
        [](void* p, size_t old_n, size_t new_n, void* ctx) -> void* {
            // Can't extend in place — allocate new, copy, old is reclaimed
            // on pool reset.
            auto* pool = static_cast<HeapResetPool*>(ctx);
            void* np = pool->allocate(new_n);
            if (np && p) std::memcpy(np, p, old_n < new_n ? old_n : new_n);
            return np;
        },
        &p
    };
}
#endif // NOTE_NO_STD_STRING == 0

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
        [](void* p, size_t old_n, size_t new_n, void* ctx) -> void* {
            auto* res = static_cast<std::pmr::memory_resource*>(ctx);
            void* np = res->allocate(new_n, alignof(std::max_align_t));
            if (np && p) {
                std::memcpy(np, p, old_n < new_n ? old_n : new_n);
                res->deallocate(p, old_n, alignof(std::max_align_t));
            }
            return np;
        },
        r
    };
}

} // namespace note
#endif
