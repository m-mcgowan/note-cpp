// Zero-allocation profiling tests for note-cpp with the buffer backend.
//
// Proves that a full execute() cycle (build request → transport → parse
// response → populate Response) requires ZERO heap allocations in steady
// state when using BufferJsonBackend + member-buffer transport.
//
// After a warm-up call (which establishes transport buffer capacity),
// subsequent execute() calls must allocate nothing.

#include <doctest.h>
#include "common/alloc_counter.hpp"

#include <note/backends/buffer.hpp>
#include <note/notecard.hpp>
#include <note/api/card_version.hpp>
#include <note/api/hub_set.hpp>
#include <note/api/note_add.hpp>
#include <note/api.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "common/scripted_transport.hpp"
using note::test::ScriptedTransport;

// ═══════════════════════════════════════════════════════════════════════════
// Tests
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("buffer/alloc_profile/zero_alloc_card_version") {
    note::backends::BufferJsonBackend<512, 64> backend;
    ScriptedTransport transport;
    transport.response = R"({"version":"notecard-7.2.1","device":"dev:12345","board":"1.0"})";

    note::Notecard nc(backend, transport);
    note::Api api(nc);

    // Warm up: first call may allocate for std::function internals
    { auto r = api.card.version().execute(); }

    // Steady-state: ZERO allocations expected
    note_tests::ScopedAllocCounter scope;
    auto r = api.card.version().execute();

    CHECK(r.has_value());
    CHECK(r.version == "notecard-7.2.1");
    CHECK(r.device == "dev:12345");
    CHECK(r.board == "1.0");
    CHECK(scope.count() == 0);
}

TEST_CASE("buffer/alloc_profile/zero_alloc_hub_set") {
    note::backends::BufferJsonBackend<512, 64> backend;
    ScriptedTransport transport;
    transport.response = R"({})";

    note::Notecard nc(backend, transport);
    note::Api api(nc);

    // Warm up
    { api.hub.set().mode("periodic").outbound(int32_t{60}).execute(); }

    note_tests::ScopedAllocCounter scope;
    auto r = api.hub.set().mode("periodic").outbound(int32_t{60}).execute();

    CHECK(r.has_value());
    CHECK(scope.count() == 0);
}

TEST_CASE("buffer/alloc_profile/zero_alloc_multiple_requests") {
    note::backends::BufferJsonBackend<512, 64> backend;
    ScriptedTransport transport;

    note::Notecard nc(backend, transport);
    note::Api api(nc);

    static constexpr const char* version_rsp = R"({"version":"v1","device":"dev:1"})";
    static constexpr const char* empty_rsp = R"({})";

    // Warm up with different request types
    transport.response = version_rsp;
    { api.card.version().execute(); }
    transport.response = empty_rsp;
    { api.hub.set().mode("periodic").execute(); }

    // Now measure 10 consecutive requests of mixed types.
    // Transport response is a const char* — zero allocation on assignment.
    note_tests::ScopedAllocCounter scope;
    for (int i = 0; i < 5; ++i) {
        transport.response = version_rsp;
        auto r1 = api.card.version().execute();
        CHECK(r1.has_value());

        transport.response = empty_rsp;
        auto r2 = api.hub.set().mode("continuous").execute();
        CHECK(r2.has_value());
    }

    CHECK(scope.count() == 0);
}

TEST_CASE("buffer/alloc_profile/zero_alloc_with_body_response") {
    // Responses with a "body" field still allocate 1 unique_ptr for the
    // sub-reader from get_object("body"). This test documents that behavior.
    note::backends::BufferJsonBackend<1024, 128> backend;
    ScriptedTransport transport;
    transport.response = R"({"version":"v1","body":{"org":"blues","product":"app"}})";

    note::Notecard nc(backend, transport);
    note::Api api(nc);

    // Warm up
    { api.card.version().execute(); }

    note_tests::ScopedAllocCounter scope;
    auto r = api.card.version().execute();

    CHECK(r.has_value());
    REQUIRE(r.body() != nullptr);
    CHECK(r.body()->get_string("org") == "blues");

    // body sub-reader costs 1 allocation (get_object returns unique_ptr)
    // NOTE: scope.count() == 1, not 0. This is intentional — the body sub-reader
    // is heap-allocated by design. Shared-counter semantics are identical to the
    // old file-local TrackingScope (both count operator new).
    CHECK(scope.count() == 1);
}

TEST_CASE("buffer/alloc_profile/zero_alloc_no_leaks") {
    note::backends::BufferJsonBackend<512, 64> backend;
    ScriptedTransport transport;
    transport.response = R"({"version":"v1"})";

    note::Notecard nc(backend, transport);
    note::Api api(nc);

    // Warm up
    { api.card.version().execute(); }

    note_tests::ScopedAllocCounter scope;
    {
        auto r = api.card.version().execute();
        CHECK(r.has_value());
        // r goes out of scope here
    }

    CHECK(scope.count() == 0);
}

TEST_CASE("buffer/alloc_profile/zero_alloc_error_response") {
    // Error responses use parse_response() (allocating) to keep error string alive.
    note::backends::BufferJsonBackend<512, 64> backend;
    ScriptedTransport transport;
    transport.response = R"({"err":"file not found"})";

    note::Notecard nc(backend, transport);
    note::Api api(nc);

    // Warm up
    transport.response = R"({})";
    { api.card.version().execute(); }

    // Error path: allocates for owned reader
    transport.response = R"({"err":"file not found"})";
    note_tests::ScopedAllocCounter scope;
    auto r = api.card.version().execute();

    CHECK(!r.has_value());
    CHECK(r.error().code == note::Error::Notecard);
    CHECK(r.error().message == "file not found");

    // Error path requires allocations to keep error message alive — that's by design.
    // NOTE: scope.count() > 0, not 0. This is intentional — the error path heap-allocates
    // an owned reader to extend the error string's lifetime beyond the transport buffer.
    CHECK(scope.count() > 0);
}
