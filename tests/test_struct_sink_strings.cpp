// Tests that StructSink populates string-typed fields with various
// string representations, not just note::string_view.
//
// Regression for: SaxAssignString originally gated on
//     std::is_convertible_v<string_view, V>
// which is `false` for std::string (its string_view ctor is explicit
// per LWG 2946). A `std::string` field silently stayed empty even
// though the wire had the right value. The fix switches to
// is_constructible_v so the explicit ctor participates.

#include <doctest.h>

#include <note/allocator.hpp>
#include <note/arena.hpp>
#include <note/json_sax.hpp>
#include <note/string_pool.hpp>
#include <note/struct_sink.hpp>

#include <cstring>
#include <string>

namespace {

struct MixedStringBody {
    note::string_view  view_field;
    std::string        string_field;
    int                int_field;
    NOTE_FIELDS(view_field, string_field, int_field)
};

struct StringOnlyBody {
    std::string a;
    std::string b;
    NOTE_FIELDS(a, b)
};

struct CharArrayBody {
    char tag[8];
    NOTE_FIELDS(tag)
};

struct SmallCharArrayBody {
    char tag[4];
    NOTE_FIELDS(tag)
};

}  // namespace

TEST_CASE("StructSink: string_view field populates directly", "[struct_sink][strings]") {
    char buf[512];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));

    MixedStringBody obj{};
    note::StructSink<MixedStringBody> sink(obj, pool);

    sink.on_string("view_field", "hello");
    sink.on_int("int_field", 42);

    REQUIRE(obj.view_field == "hello");
    REQUIRE(obj.int_field == 42);
}

TEST_CASE("StructSink: std::string field populates (regression)", "[struct_sink][strings]") {
    char buf[512];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));

    MixedStringBody obj{};
    note::StructSink<MixedStringBody> sink(obj, pool);

    // Pre-fix: this was silently dropped because
    // std::is_convertible_v<string_view, std::string> is false.
    sink.on_string("string_field", "world");

    REQUIRE(obj.string_field == "world");
}

TEST_CASE("StructSink: char[N] field receives null-terminated copy", "[struct_sink][strings]") {
    char buf[512];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));

    CharArrayBody obj{};
    note::StructSink<CharArrayBody> sink(obj, pool);

    sink.on_string("tag", "hello");
    REQUIRE(std::string(obj.tag) == "hello");
    REQUIRE(obj.tag[5] == '\0');   // explicit null terminator
}

TEST_CASE("StructSink: char[N] truncates oversize values and null-terminates",
          "[struct_sink][strings]") {
    char buf[512];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));

    SmallCharArrayBody obj{};
    note::StructSink<SmallCharArrayBody> sink(obj, pool);

    sink.on_string("tag", "hello");   // 5 chars into 4-byte buffer
    REQUIRE(obj.tag[0] == 'h');
    REQUIRE(obj.tag[1] == 'e');
    REQUIRE(obj.tag[2] == 'l');
    REQUIRE(obj.tag[3] == '\0');       // truncated + null
}

TEST_CASE("StructSink: std::string survives past transport buffer reuse",
          "[struct_sink][strings]") {
    char buf[512];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));

    StringOnlyBody obj{};
    note::StructSink<StringOnlyBody> sink(obj, pool);

    // Populate from transient string_views backed by a stack buffer.
    char wire[16];
    std::strcpy(wire, "value-A");
    sink.on_string("a", note::string_view(wire));
    std::strcpy(wire, "value-B");  // overwrite the source bytes
    sink.on_string("b", note::string_view(wire));

    // std::string owns its own storage, so the first field must be
    // unaffected by the source buffer reuse.
    REQUIRE(obj.a == "value-A");
    REQUIRE(obj.b == "value-B");
}
