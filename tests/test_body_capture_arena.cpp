// Tests that body SAX events are forwarded to a BodyHandler when set.
//
// Verifies:
// - body events are forwarded to StructSink via BodyHandler
// - body events are silently skipped when no handler is set
// - reset() clears body depth tracking
#include <doctest.h>
#include <note/arena.hpp>
#include <note/allocator.hpp>
#include <note/string_pool.hpp>
#include <note/struct_sink.hpp>
#include <note/json_sax.hpp>
#include <note/api/env_get.hpp>
#include <cstring>

namespace {

struct EnvBody {
    float temp;
    note::string_view label;
    NOTE_FIELDS(temp, label)
};

} // namespace

TEST_CASE("Sink forwards body events to BodyHandler") {
    char buf[2048];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));

    note::api::EnvGet::Response rsp{};
    note::api::EnvGet::Response::Sink sink(rsp, pool);

    // Create a body struct and handler
    EnvBody body{};
    note::StructSink<EnvBody> body_sink(body, pool);
    auto handler = note::make_body_handler(body_sink);
    sink.set_body_handler(handler);

    // Simulate SAX events for: {"text":"hello","body":{"temp":22.5,"label":"room"}}
    sink.on_string("text", "hello");
    sink.on_object_begin("body");
    sink.on_number("temp", "22.5");
    sink.on_string("label", "room");
    sink.on_object_end("body");

    REQUIRE(rsp.text == "hello");
    REQUIRE(body.temp == 22.5f);
    REQUIRE(body.label == "room");
}

TEST_CASE("Sink skips body events when no handler is set") {
    char buf[1024];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));

    note::api::EnvGet::Response rsp{};
    note::api::EnvGet::Response::Sink sink(rsp, pool);

    // No body handler set — body events should be silently skipped
    sink.on_string("text", "hello");
    sink.on_object_begin("body");
    sink.on_number("temp", "22.5");
    sink.on_string("label", "room");
    sink.on_object_end("body");

    REQUIRE(rsp.text == "hello");
    // No crash, no body data captured
}

TEST_CASE("Sink reset clears body depth tracking") {
    char buf[1024];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));

    note::api::EnvGet::Response rsp{};
    note::api::EnvGet::Response::Sink sink(rsp, pool);

    EnvBody body{};
    note::StructSink<EnvBody> body_sink(body, pool);
    sink.set_body_handler(note::make_body_handler(body_sink));

    sink.on_object_begin("body");
    sink.on_string("label", "test");
    sink.on_object_end("body");

    sink.reset();
    // After reset, body_depth_ should be 0 and we can parse again
    sink.on_string("text", "after-reset");
    REQUIRE(rsp.text == "after-reset");
}
