// SAX-path zero-allocation profiling tests for note-cpp.
//
// Proves that execute() with a configured Allocator (SAX path) performs
// ZERO heap allocations — all response strings are interned into the arena.
// This complements test_alloc_profile.cpp which tests the tree-parse path.

#include <doctest.h>
#include "common/alloc_counter.hpp"

#include <note/backends/buffer.hpp>
#include <note/notecard.hpp>
#include <note/arena.hpp>
#include <note/allocator.hpp>
#include <note/api/card_version.hpp>
#include <note/api/hub_set.hpp>
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

TEST_CASE("buffer/sax_alloc_profile/sax_zero_alloc_card_version") {
    note::backends::StaticJsonBackend<512, 64> backend;
    ScriptedTransport transport;
    transport.response = R"({"version":"notecard-7.2.1","device":"dev:12345","board":"1.0","cell":true})";

    char arena_buf[1024];
    note::MonotonicArena arena(arena_buf, sizeof(arena_buf));

    note::Notecard nc(backend, transport);
    nc.set_allocator(note::arena_allocator(arena));
    note::Api api(nc);

    // Warm up: first call may allocate for std::function internals
    { auto r = api.card.version().execute(); arena.reset(); }

    // Steady-state: ZERO heap allocations expected
    note_tests::ScopedAllocCounter scope;
    auto r = api.card.version().execute();

    CHECK(r.has_value());
    CHECK(r.version == "notecard-7.2.1");
    CHECK(r.device == "dev:12345");
    CHECK(r.board == "1.0");
    CHECK(r.cell == true);
    CHECK(scope.count() == 0);
}

TEST_CASE("buffer/sax_alloc_profile/sax_string_survives_reuse") {
    note::backends::StaticJsonBackend<512, 64> backend;
    ScriptedTransport transport;

    char arena_buf[2048];
    note::MonotonicArena arena(arena_buf, sizeof(arena_buf));

    note::Notecard nc(backend, transport);
    nc.set_allocator(note::arena_allocator(arena));
    note::Api api(nc);

    // Warm up
    transport.response = R"({})";
    { api.hub.set().execute(); }

    // First request: save version string
    transport.response = R"({"version":"firmware-1.0","device":"dev:AAA"})";
    auto r1 = api.card.version().execute();
    CHECK(r1.has_value());

    // Save copies of the string data pointers before second request
    const char* v1_data = r1.version.data();
    const char* d1_data = r1.device.data();

    // Second request: transport buffer overwritten
    transport.response = R"({"version":"firmware-2.0","device":"dev:BBB"})";
    auto r2 = api.card.version().execute();
    CHECK(r2.has_value());

    // r1's strings survive because they're in the arena, not the transport buffer
    CHECK(r1.version == "firmware-1.0");
    CHECK(r1.device == "dev:AAA");
    CHECK(r1.version.data() == v1_data);  // same arena pointer
    CHECK(r1.device.data() == d1_data);

    // r2 has its own arena-backed copies
    CHECK(r2.version == "firmware-2.0");
    CHECK(r2.device == "dev:BBB");
}

TEST_CASE("buffer/sax_alloc_profile/sax_error_detection") {
    note::backends::StaticJsonBackend<512, 64> backend;
    ScriptedTransport transport;

    char arena_buf[1024];
    note::MonotonicArena arena(arena_buf, sizeof(arena_buf));

    note::Notecard nc(backend, transport);
    nc.set_allocator(note::arena_allocator(arena));
    note::Api api(nc);

    // Warm up
    transport.response = R"({})";
    { api.hub.set().execute(); }

    // Error response
    transport.response = R"({"err":"file not found"})";
    note_tests::ScopedAllocCounter scope;
    auto r = api.card.version().execute();

    CHECK(!r.has_value());
    CHECK(r.error().code == note::Error::Notecard);
    CHECK(r.error().message == "file not found");

    // error string interned in arena, not heap
    CHECK(scope.count() == 0);
}

TEST_CASE("buffer/sax_alloc_profile/sax_json_parse_error") {
    note::backends::StaticJsonBackend<512, 64> backend;
    ScriptedTransport transport;

    char arena_buf[1024];
    note::MonotonicArena arena(arena_buf, sizeof(arena_buf));

    note::Notecard nc(backend, transport);
    nc.set_allocator(note::arena_allocator(arena));
    note::Api api(nc);

    // Warm up
    transport.response = R"({})";
    { api.hub.set().execute(); }

    // Malformed JSON
    transport.response = R"({invalid json)";
    auto r = api.card.version().execute();

    CHECK(!r.has_value());
    CHECK(r.error().code == note::Error::Json);
}

TEST_CASE("buffer/sax_alloc_profile/sax_bounded_memory") {
    note::backends::StaticJsonBackend<512, 64> backend;
    ScriptedTransport transport;
    transport.response = R"({"version":"notecard-7.2.1","device":"dev:12345","board":"1.0"})";

    char arena_buf[4096];
    note::MonotonicArena arena(arena_buf, sizeof(arena_buf));

    note::Notecard nc(backend, transport);
    nc.set_allocator(note::arena_allocator(arena));
    note::Api api(nc);

    // Warm up
    { auto r = api.card.version().execute(); arena.reset(); }

    // Measure arena usage for one request
    size_t used_after_one = 0;
    {
        auto r = api.card.version().execute();
        used_after_one = arena.used();
        arena.reset();
    }

    // Do N requests with arena reset between each — peak memory should be stable
    nc.set_allocator(note::arena_allocator(arena));
    for (int i = 0; i < 100; ++i) {
        auto r = api.card.version().execute();
        CHECK(r.has_value());
        CHECK(arena.used() == used_after_one);
        arena.reset();
        nc.set_allocator(note::arena_allocator(arena));
    }
}

TEST_CASE("buffer/sax_alloc_profile/sax_via_api") {
    // Full Api → Notecard flow with stored allocator
    note::backends::StaticJsonBackend<512, 64> backend;
    ScriptedTransport transport;
    transport.response = R"({"version":"v3.5.1","device":"dev:XYZ","name":"notecard"})";

    char arena_buf[1024];
    note::MonotonicArena arena(arena_buf, sizeof(arena_buf));

    note::Notecard nc(backend, transport);
    nc.set_allocator(note::arena_allocator(arena));

    note::Api api(nc);

    // Warm up
    { api.card.version().execute(); arena.reset(); nc.set_allocator(note::arena_allocator(arena)); }

    note_tests::ScopedAllocCounter scope;
    auto r = api.execute(api.card.version());

    CHECK(r.has_value());
    CHECK(r.version == "v3.5.1");
    CHECK(r.device == "dev:XYZ");
    CHECK(r.name == "notecard");
    CHECK(scope.count() == 0);
}

TEST_CASE("buffer/sax_alloc_profile/sax_explicit_allocator") {
    // Per-call allocator overload (no stored allocator on Notecard)
    note::backends::StaticJsonBackend<512, 64> backend;
    ScriptedTransport transport;
    transport.response = R"({"version":"v1.0","device":"dev:001"})";

    note::Notecard nc(backend, transport);
    // No set_allocator() — using explicit per-call overload

    char arena_buf[1024];
    note::MonotonicArena arena(arena_buf, sizeof(arena_buf));

    // Warm up (tree path, since no allocator configured)
    { nc.execute(note::api::CardVersion{}); }

    note_tests::ScopedAllocCounter scope;
    auto r = nc.execute(note::api::CardVersion{}, note::arena_allocator(arena));

    CHECK(r.has_value());
    CHECK(r.version == "v1.0");
    CHECK(r.device == "dev:001");
    CHECK(scope.count() == 0);
}
