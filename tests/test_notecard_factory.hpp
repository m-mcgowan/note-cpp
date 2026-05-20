#pragma once

/// @file test_notecard_factory.hpp
/// Factory for creating Notecard instances in tests.
///
/// Production Notecards have request IDs enabled by default (for log
/// correlation). Tests that check exact wire format should use
/// make_test_notecard() which disables IDs.

#include <note/notecard.hpp>

#include <memory>

#if !NOTE_NO_POLYMORPHIC

namespace note::test {

/// Create a Notecard configured for testing: request IDs disabled,
/// retry policy with zero retries (single-attempt, deterministic).
inline Notecard make_test_notecard(JsonBackend& backend, ITransact& transport) {
    Notecard nc(backend, transport);
    nc.set_request_ids(false);
    nc.set_retry_policy({.max_retries = 0});
    return nc;
}

/// Streaming variant.
inline Notecard make_test_notecard(Protocol& transport, Allocator alloc = {}) {
    Notecard nc(transport, alloc);
    nc.set_request_ids(false);
    nc.set_retry_policy({.max_retries = 0});
    return nc;
}

/// Heap-allocated test Notecard. Use when running on MCUs whose loop
/// task stack is too small to host a value-typed Notecard alongside the
/// test's other locals. The Notecard footprint (~1.4 KB) moves to heap;
/// stack pays for a `unique_ptr` only.
inline std::unique_ptr<Notecard> make_test_notecard_heap(
        JsonBackend& backend, ITransact& transport) {
    auto nc = std::make_unique<Notecard>(backend, transport);
    nc->set_request_ids(false);
    nc->set_retry_policy({.max_retries = 0});
    return nc;
}

/// Heap-allocated streaming variant.
inline std::unique_ptr<Notecard> make_test_notecard_heap(
        Protocol& transport, Allocator alloc = {}) {
    auto nc = std::make_unique<Notecard>(transport, alloc);
    nc->set_request_ids(false);
    nc->set_retry_policy({.max_retries = 0});
    return nc;
}

} // namespace note::test

#endif // NOTE_NO_POLYMORPHIC
