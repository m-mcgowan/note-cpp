// StaticArena — fixed-size MonotonicArena pre-sized for a RequestSet.
//
// Bundles the buffer, alignment, MonotonicArena, and compile-time size
// derivation into a single object. Replaces the four-line idiom
//
//     static constexpr size_t kArenaSize = MyRequests::max_arena_size;
//     alignas(...) static char arena_buf[kArenaSize];
//     static note::MonotonicArena arena(arena_buf);
//
// with
//
//     static note::StaticArena<MyRequests> arena;
//
// Implicitly converts to MonotonicArena&, so it drops straight into
// note::arena_allocator(arena) and any other API that takes the base type.
#pragma once

#include <cstddef>
#include <new>

#include <note/arena.hpp>
#include <note/request_set.hpp>

namespace note {

template<typename RequestSetT>
class StaticArena {
public:
    static constexpr size_t size = RequestSetT::max_arena_size;
    static_assert(
        size > 0,
        "StaticArena: RequestSet has zero max_arena_size — every request type "
        "in the set has a void response, so no arena is needed. Drop the arena "
        "entirely, or add a request type with a non-void response.");

    // GCC 13+ flags `arena_(buf_)` (calling the `char(&)[N]` overload) as
    // "member buf_ used uninitialized" because reference-binding to the
    // array makes GCC's flow analysis treat the contents as a read. The
    // pointer+capacity overload uses array-to-pointer decay, which only
    // takes the address — no read, no warning — and avoids the memset
    // cost of `buf_[size]{}` value-init on AVR.
    StaticArena() : arena_(buf_, size) {}

    // Implicit conversion so StaticArena drops into existing APIs that take
    // a MonotonicArena&.
    operator MonotonicArena&() noexcept { return arena_; }
    MonotonicArena& base() noexcept { return arena_; }

    void reset() { arena_.reset(); }
    size_t used() const { return arena_.used(); }
    size_t capacity() const { return arena_.capacity(); }
    size_t available() const { return arena_.available(); }

private:
    alignas(alignof(std::max_align_t)) char buf_[size];
    MonotonicArena arena_;
};

} // namespace note
