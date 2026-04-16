#pragma once

/// @file test_notecard_factory.hpp
/// Factory for creating Notecard instances in tests.
///
/// Production Notecards have request IDs enabled by default (for log
/// correlation). Tests that check exact wire format should use
/// make_test_notecard() which disables IDs.

#include <note/notecard.hpp>

#if !NOTE_NO_POLYMORPHIC

namespace note::test {

/// Create a Notecard configured for testing: request IDs disabled,
/// retry policy with zero retries (single-attempt, deterministic).
inline Notecard make_test_notecard(JsonBackend& backend, IBufferedTransport& transport) {
    Notecard nc(backend, transport);
    nc.set_request_ids(false);
    nc.set_retry_policy({.max_retries = 0});
    return nc;
}

/// Streaming variant.
inline Notecard make_test_notecard(IStreamingTransport& transport, Allocator alloc = {}) {
    Notecard nc(transport, alloc);
    nc.set_request_ids(false);
    nc.set_retry_policy({.max_retries = 0});
    return nc;
}

} // namespace note::test

#endif // NOTE_NO_POLYMORPHIC
