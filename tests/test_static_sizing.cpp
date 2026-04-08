#include "catch.hpp"
#include <note/request_set.hpp>
#include <note/api/card_status.hpp>
#include <note/api/card_version.hpp>
#include <note/api/card_io.hpp>
#include <note/api/hub_set.hpp>
#include <note/api/note_add.hpp>
#include <note/arena.hpp>
#include <note/allocator.hpp>
#include <note/string_pool.hpp>

TEST_CASE("RequestSet::max_arena_size is max of component responses") {
    using R = note::RequestSet<
        note::api::CardStatus,
        note::api::CardVersion,
        note::api::HubSet
    >;

    constexpr size_t sz = R::max_arena_size;

    // Must be at least as large as each component
    STATIC_REQUIRE(sz >= note::api::CardStatus::Response::max_arena_size);
    STATIC_REQUIRE(sz >= note::api::CardVersion::Response::max_arena_size);
    // HubSet has void Response — contributes 0
}

TEST_CASE("RequestSet with void-response types") {
    // HubSet and CardIo both have using Response = void — contribute 0
    using R = note::RequestSet<note::api::HubSet, note::api::CardIo>;
    constexpr size_t sz = R::max_arena_size;
    STATIC_REQUIRE(sz == 0);
}

TEST_CASE("Single-request RequestSet") {
    using R = note::RequestSet<note::api::CardStatus>;
    constexpr size_t sz = R::max_arena_size;
    STATIC_REQUIRE(sz == note::api::CardStatus::Response::max_arena_size);
}

TEST_CASE("max_arena_size values are reasonable") {
    // CardStatus: 1 string field (status: 80) + err (64)
    constexpr size_t cs = note::api::CardStatus::Response::max_arena_size;
    STATIC_REQUIRE(cs >= 128);
    STATIC_REQUIRE(cs < 1024);

    // CardVersion: multiple string fields + body
    constexpr size_t cv = note::api::CardVersion::Response::max_arena_size;
    STATIC_REQUIRE(cv > cs);  // more fields = larger
    STATIC_REQUIRE(cv < 4096);
}

TEST_CASE("Arena usage fits max_arena_size for CardStatus") {
    constexpr size_t sz = note::api::CardStatus::Response::max_arena_size;
    char buf[sz];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));

    note::api::CardStatus::Response rsp{};
    note::api::CardStatus::Response::Sink sink(rsp, pool);

    // Simulate worst-case: longest plausible status string
    sink.on_string("status", "idle {connected} {network-off} extra padding for safety margins");
    sink.on_bool("connected", true);
    sink.on_number("time", "1712345678");

    REQUIRE(arena.used() <= sz);
    REQUIRE(rsp.status.value() == "idle {connected} {network-off} extra padding for safety margins");
    REQUIRE(rsp.connected.value() == true);
}

TEST_CASE("Arena usage fits max_arena_size for CardVersion (body endpoint)") {
    constexpr size_t sz = note::api::CardVersion::Response::max_arena_size;
    char buf[sz];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));

    note::api::CardVersion::Response rsp{};
    note::api::CardVersion::Response::Sink sink(rsp, pool);

    // Simulate response with multiple string fields
    sink.on_string("version", "notecard-7.4.1.17591");
    sink.on_string("device", "dev:860322068097069");
    sink.on_string("board", "1.22");
    sink.on_string("sku", "NOTE-NBGL-500");
    sink.on_string("name", "my-notecard-device");

    // Body capture
    sink.on_object_begin("body");
    sink.on_string("org", "Blues");
    sink.on_string("product", "com.example.app");
    sink.on_object_end("body");

    REQUIRE(arena.used() <= sz);
    REQUIRE(rsp.version.value() == "notecard-7.4.1.17591");
}

// ── Arena exhaustion tests ──────────────────────────────────────────────────

TEST_CASE("StringPool returns empty string_view on arena exhaustion") {
    char buf[8];  // tiny arena
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));

    // First intern should succeed if it fits
    auto s1 = pool.intern("hi");
    REQUIRE(!s1.empty());
    // Eventually the arena will run out
    auto s2 = pool.intern("this string is way too long for an 8 byte arena");
    // Exhausted: intern returns empty
    REQUIRE(s2.empty());
}

TEST_CASE("Arena allocate returns nullptr on exhaustion") {
    char buf[4];
    note::MonotonicArena arena(buf);

    void* p1 = arena.allocate(2);
    REQUIRE(p1 != nullptr);

    void* p2 = arena.allocate(16);  // won't fit
    REQUIRE(p2 == nullptr);
}

// GenericResponseSink uses offsetof which requires standard-layout ResponseField
// (no Printable vtable). Only valid when NOTE_PRINTABLE=0.
#if NOTE_PRINTABLE == 0
TEST_CASE("GenericResponseSink handles arena exhaustion gracefully") {
    // Use a 1-byte arena — too small for any string interning.
    char buf[1];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));

    note::api::NoteAdd::Response rsp{};
    note::GenericResponseSink gsink{
        &rsp,
        note::api::NoteAdd::field_descs_ptr(),
        note::api::NoteAdd::field_count,
        &pool
    };

    // String field — interning will fail, field should be empty
    gsink.on_string("note", "sensors.qo");
    REQUIRE(rsp.noteId.value().empty());

    // Non-string fields should still work (no arena needed)
    gsink.on_bool("template", true);
    REQUIRE(rsp.template_.value() == true);

    gsink.on_int("total", 42);
    REQUIRE(rsp.total.value() == 42);
}
#endif
