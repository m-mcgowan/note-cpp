// Tests that BodyCaptureSink allocates body JSON into the arena (not heap).
//
// Verifies:
// - body_json data pointer falls within the arena buffer
// - arena.used() > 0 after body capture
// - reset() clears all body state
#include "catch.hpp"
#include <note/arena.hpp>
#include <note/allocator.hpp>
#include <note/string_pool.hpp>
#include <note/json_sax.hpp>
#include <note/api/env_get.hpp>
#include <cstring>

TEST_CASE("BodyCaptureSink uses arena, not heap") {
    char buf[2048];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));

    note::api::EnvGet::Response rsp{};
    note::api::EnvGet::Response::Sink sink(rsp, pool);

    // Simulate SAX events for: {"text":"hello","body":{"temp":22.5,"label":"room"}}
    sink.on_string("text", "hello");
    sink.on_object_begin("body");
    sink.on_number("temp", "22.5");
    sink.on_string("label", "room");
    sink.on_object_end("body");

    REQUIRE(sink.body_json == R"({"temp":22.5,"label":"room"})");
    REQUIRE(arena.used() > 0);

    // Verify the body_json data pointer is within the arena buffer
    auto* body_ptr = sink.body_json.data();
    REQUIRE(body_ptr >= buf);
    REQUIRE(body_ptr < buf + sizeof(buf));
}

TEST_CASE("BodyCaptureSink reset clears body state") {
    char buf[1024];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));

    note::api::EnvGet::Response rsp{};
    note::api::EnvGet::Response::Sink sink(rsp, pool);

    sink.on_object_begin("body");
    sink.on_string("k", "v");
    sink.on_object_end("body");
    REQUIRE(!sink.body_json.empty());

    sink.reset();
    REQUIRE(sink.body_json.empty());
}
