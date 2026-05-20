// Monotonic (bump-pointer) arena allocator for note-cpp.
//
// Allocates from a caller-provided buffer with zero heap usage.
// Individual deallocations are no-ops — call reset() to reclaim all memory.
// Ideal for request/response cycles where everything is allocated then freed together.
//
// Usage:
//   char pool[4096];
//   note::MonotonicArena arena(pool);
//   void* p = arena.allocate(128);  // bump pointer
//   arena.reset();                  // reclaim everything
#pragma once

#include <cstddef>
#include <cstdint>
#include <new>

namespace note {

class MonotonicArena {
public:
    MonotonicArena(char* buf, size_t capacity)
        : buf_(buf), capacity_(capacity) {}

    template<size_t N>
    explicit MonotonicArena(char (&buf)[N])
        : buf_(buf), capacity_(N) {}

    // Allocate size bytes with max alignment. Returns nullptr if exhausted.
    void* allocate(size_t size) {
        size_t aligned = (offset_ + kAlign - 1) & ~(kAlign - 1);
        if (aligned + size > capacity_) return nullptr;
        void* p = buf_ + aligned;
        offset_ = aligned + size;
        return p;
    }

    // No-op — arena reclaims everything on reset().
    void deallocate(void*) {}

    // Reclaim all memory. All previous allocations become invalid.
    void reset() { offset_ = 0; }

    size_t used() const { return offset_; }
    size_t capacity() const { return capacity_; }
    size_t available() const { return capacity_ - offset_; }

private:
    static constexpr size_t kAlign = alignof(std::max_align_t);

    char* buf_;
    size_t capacity_;
    size_t offset_ = 0;
};

namespace detail {

/// Compile-time arena allocation cost: size rounded up to arena alignment.
/// Used by generated Response::max_arena_size constants.
constexpr size_t arena_cost(size_t n) {
    constexpr size_t kAlign = alignof(std::max_align_t);
    return n == 0 ? 0 : (n + kAlign - 1) & ~(kAlign - 1);
}

} // namespace detail

} // namespace note
