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
//
// HEAP ALERT: this header is the only place in the typed-API stack that
// reaches `std::malloc` / `std::free`. It lives in a separate header so
// `note/arena.hpp` and its consumers stay genuinely heap-free — pull
// this in only when you explicitly want heap-backed arena semantics.
// The class itself is gated by `NOTE_NO_STD_STRING == 0` so AVR/MINIMAL
// builds never compile it.
#pragma once

#include <note/note_config.hpp>

#if NOTE_NO_STD_STRING == 0

#include <cstddef>
#include <cstdlib>
#include <new>

namespace note {

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

} // namespace note

#endif // NOTE_NO_STD_STRING == 0
