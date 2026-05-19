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

#include <note/note_config.hpp>
#if NOTE_NO_STD_STRING == 0
// HeapResetPool — malloc/free with batched lifecycle. The malloc-backed
// counterpart to MonotonicArena: every allocate() goes to std::malloc,
// every deallocate() is a no-op, and reset() (or destruction) frees the
// whole batch in one pass.
//
// Use when you want arena-style "drain on reset" semantics but don't want
// to size a buffer up front. Common shape on desktop / Linux hosts where
// the heap is fine but you still want predictable lifetime control.
//
// Lifetime guarantee: every pointer returned by allocate() stays valid
// until reset() runs or the pool is destroyed. After reset(), all of them
// are invalid.
class HeapResetPool {
public:
    HeapResetPool() = default;
    HeapResetPool(const HeapResetPool&) = delete;
    HeapResetPool& operator=(const HeapResetPool&) = delete;
    HeapResetPool(HeapResetPool&& o) noexcept : head_(o.head_) { o.head_ = nullptr; }
    HeapResetPool& operator=(HeapResetPool&& o) noexcept {
        if (this != &o) { reset(); head_ = o.head_; o.head_ = nullptr; }
        return *this;
    }
    ~HeapResetPool() { reset(); }

    void* allocate(size_t size) {
        // Intrusive header in each block: a next pointer that links blocks
        // back into the pool. Caller's payload starts after the header,
        // aligned to max_align_t.
        void* raw = std::malloc(sizeof(Block) + size);
        if (!raw) return nullptr;
        auto* b = static_cast<Block*>(raw);
        b->next = head_;
        head_ = b;
        return b->payload();
    }

    // No-op — pool reclaims everything on reset().
    void deallocate(void*) {}

    // Free every outstanding allocation. All payload pointers become invalid.
    void reset() {
        while (head_) {
            Block* next = head_->next;
            std::free(head_);
            head_ = next;
        }
    }

private:
    struct Block {
        Block* next;
        // Payload starts here, aligned to max_align_t.
        alignas(std::max_align_t) char data[1];
        char* payload() { return data; }
    };
    Block* head_ = nullptr;
};
#endif // NOTE_NO_STD_STRING == 0

} // namespace note
