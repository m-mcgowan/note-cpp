// Tests for compile-time body template (note::body_template).
//
// Scope:
//   - int32 / double / bool / string slots, plus nested object ($No) and
//     array ($Na) slots
//   - positional $1/$2/... markers (no slot-name parsing)
//   - nested body_object / body_array as field values and array elements
//   - both wire formats: JSONB opcodes (NOTE_JSONB) and JSON text (default)
//
// Architecture: the template produces a static byte pool + N+1 segments +
// N slot types at compile time. `.with(...)` captures runtime values;
// emit_to(writer) walks segments interleaved with per-slot value emits.
//
// Validation strategy: emit the body's byte stream to a capturing writer
// and compare to what a hand-rolled lambda produces through the canonical
// streaming builder for the active wire format (`wire_build`). If the bytes
// match, the parse + emit are correct. A handful of literal-string checks
// (under non-JSONB) pin the exact JSON text independently of that reference.
//
// The whole suite runs in both modes; the same binary is compiled twice
// (default = JSON text, -DNOTE_JSONB=1 = JSONB opcodes).

#include <doctest.h>

#include <note/body.hpp>
#include <note/body_template.hpp>
#include <note/jsonb.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

using namespace note;

namespace {

struct ByteCapture : JsonWriter {
    std::vector<uint8_t> bytes;
    bool write(const char* data, size_t len) override {
        for (size_t i = 0; i < len; ++i)
            bytes.push_back(static_cast<uint8_t>(data[i]));
        return true;
    }
};

// Build a reference body through the canonical streaming builder for the
// active wire format: StreamingJsonbBuilder under NOTE_JSONB, the JSON-text
// StreamingJsonBuilder otherwise. body_template's emit must byte-match this
// in whichever mode the suite is compiled.
template<typename BuildFn>
std::vector<uint8_t> wire_build(BuildFn fn) {
    ByteCapture cap;
#if NOTE_JSONB == 1
    StreamingJsonbBuilder b(cap);
#else
    StreamingJsonBuilder b(cap);
#endif
    fn(b);
    b.to_view();  // close root object
    return cap.bytes;
}

/// Walk the template's segments + values, writing bytes into a capture.
/// Equivalent to "emit the template standalone".
template<typename Call>
std::vector<uint8_t> capture_bytes(const Call& call) {
    ByteCapture cap;
    call.emit_to(cap);
    return cap.bytes;
}

/// Bytes a BodyValue produces when spliced into a fresh request object,
/// in the active wire format. Exercises the `req.body(...)` path.
inline std::vector<uint8_t> body_value_bytes(const BodyValue& bv) {
    ByteCapture cap;
#if NOTE_JSONB == 1
    StreamingJsonbBuilder outer(cap);
#else
    StreamingJsonBuilder outer(cap);
#endif
    bv.write_to(outer);
    outer.to_view();
    return cap.bytes;
}

}  // anonymous namespace

TEST_CASE("body_template: empty object") {
    constexpr auto tpl = body_template<R"({})">();
    auto bytes = capture_bytes(tpl.with());
    auto expected = wire_build([](JsonBuilder&) {});
    REQUIRE(bytes.size() == expected.size());
    CHECK(bytes == expected);
}

TEST_CASE("body_template: single int32 slot") {
    constexpr auto tpl = body_template<R"({"a":$1})">();
    auto bytes = capture_bytes(tpl.with(int32_t{42}));
    auto expected = wire_build([](JsonBuilder& b) {
        b.add("a", int32_t{42});
    });
    REQUIRE(bytes.size() == expected.size());
    CHECK(bytes == expected);
}

TEST_CASE("body_template: two int32 slots") {
    constexpr auto tpl = body_template<R"({"a":$1,"b":$2})">();
    auto bytes = capture_bytes(tpl.with(int32_t{1}, int32_t{2}));
    auto expected = wire_build([](JsonBuilder& b) {
        b.add("a", int32_t{1});
        b.add("b", int32_t{2});
    });
    REQUIRE(bytes.size() == expected.size());
    CHECK(bytes == expected);
}

TEST_CASE("body_template: negative int32 round-trips through LE32 patch") {
    constexpr auto tpl = body_template<R"({"v":$1})">();
    auto bytes = capture_bytes(tpl.with(int32_t{-1}));
    auto expected = wire_build([](JsonBuilder& b) {
        b.add("v", int32_t{-1});
    });
    REQUIRE(bytes.size() == expected.size());
    CHECK(bytes == expected);
}

TEST_CASE("body_template: whitespace inside template literal is ignored") {
    constexpr auto tpl = body_template<R"(
        {
            "x" : $1 ,
            "y" : $2
        }
    )">();
    auto bytes = capture_bytes(tpl.with(int32_t{10}, int32_t{20}));
    auto expected = wire_build([](JsonBuilder& b) {
        b.add("x", int32_t{10});
        b.add("y", int32_t{20});
    });
    REQUIRE(bytes.size() == expected.size());
    CHECK(bytes == expected);
}

TEST_CASE("body_template: slot order is positional ($1 first, then $2…)") {
    constexpr auto tpl = body_template<R"({"a":$1,"b":$2})">();
    auto bytes = capture_bytes(tpl.with(int32_t{100}, int32_t{200}));
    auto expected = wire_build([](JsonBuilder& b) {
        b.add("a", int32_t{100});
        b.add("b", int32_t{200});
    });
    CHECK(bytes == expected);
}

TEST_CASE("body_template: compile-time shape is observable") {
    constexpr auto tpl = body_template<R"({"k":$1})">();
    using T = decltype(tpl);
    static_assert(T::sizes_.slot_count == 1);
    static_assert(T::data_.slot_types[0] == detail::slot_type::Int32);
#if NOTE_JSONB == 1
    // Static pool: kBeginObject(1) + kItem+"k\0"(3) + kInt32(1) + kEndObject(1) = 6
    static_assert(T::sizes_.static_byte_count == 6);
    static_assert(T::data_.static_pool[0] == jsonb::kBeginObject);
    // segments: pre-slot (kBeginObject+kItem+"k\0"+kInt32 = 5), post-slot (kEndObject = 1)
    static_assert(T::data_.segments[0].length == 5);
    static_assert(T::data_.segments[1].length == 1);
#else
    // Static pool: `{"k":`(5) + int (no opcode) + `}`(1) = 6
    static_assert(T::sizes_.static_byte_count == 6);
    static_assert(T::data_.static_pool[0] == static_cast<uint8_t>('{'));
    // segments: pre-slot (`{"k":` = 5), post-slot (`}` = 1)
    static_assert(T::data_.segments[0].length == 5);
    static_assert(T::data_.segments[1].length == 1);
#endif
}

// ---------------------------------------------------------------------------
// Double slots — `$Nf` syntax → static kDouble + 8 host-LE runtime bytes
// ---------------------------------------------------------------------------

TEST_CASE("body_template: single double slot") {
    constexpr auto tpl = body_template<R"({"t":$1f})">();
    auto bytes = capture_bytes(tpl.with(22.5));
    auto expected = wire_build([](JsonBuilder& b) {
        b.add("t", 22.5);
    });
    REQUIRE(bytes.size() == expected.size());
    CHECK(bytes == expected);
}

TEST_CASE("body_template: double slot accepts float (widens to double)") {
    constexpr auto tpl = body_template<R"({"t":$1f})">();
    auto bytes = capture_bytes(tpl.with(22.5f));
    auto expected = wire_build([](JsonBuilder& b) {
        b.add("t", static_cast<double>(22.5f));
    });
    REQUIRE(bytes.size() == expected.size());
    CHECK(bytes == expected);
}

TEST_CASE("body_template: int32 + double mixed slots") {
    constexpr auto tpl = body_template<R"({"seq":$1,"temp":$2f})">();
    auto bytes = capture_bytes(tpl.with(int32_t{7}, -3.14));
    auto expected = wire_build([](JsonBuilder& b) {
        b.add("seq", int32_t{7});
        b.add("temp", -3.14);
    });
    REQUIRE(bytes.size() == expected.size());
    CHECK(bytes == expected);
}

// ---------------------------------------------------------------------------
// Bool slots — `$Nb` syntax → single runtime byte (kTrue or kFalse)
// ---------------------------------------------------------------------------

TEST_CASE("body_template: bool slot true") {
    constexpr auto tpl = body_template<R"({"on":$1b})">();
    auto bytes = capture_bytes(tpl.with(true));
    auto expected = wire_build([](JsonBuilder& b) {
        b.add("on", true);
    });
    REQUIRE(bytes.size() == expected.size());
    CHECK(bytes == expected);
}

TEST_CASE("body_template: bool slot false") {
    constexpr auto tpl = body_template<R"({"on":$1b})">();
    auto bytes = capture_bytes(tpl.with(false));
    auto expected = wire_build([](JsonBuilder& b) {
        b.add("on", false);
    });
    REQUIRE(bytes.size() == expected.size());
    CHECK(bytes == expected);
}

TEST_CASE("body_template: all three numeric/bool types mixed") {
    constexpr auto tpl = body_template<
        R"({"seq":$1,"temp":$2f,"alarm":$3b})">();
    auto bytes = capture_bytes(tpl.with(int32_t{42}, 18.0, true));
    auto expected = wire_build([](JsonBuilder& b) {
        b.add("seq", int32_t{42});
        b.add("temp", 18.0);
        b.add("alarm", true);
    });
    REQUIRE(bytes.size() == expected.size());
    CHECK(bytes == expected);
}

// ---------------------------------------------------------------------------
// String slots — `$Ns` syntax → static kString + variable runtime bytes + '\0'
// ---------------------------------------------------------------------------

TEST_CASE("body_template: string slot from const char*") {
    constexpr auto tpl = body_template<R"({"name":$1s})">();
    auto bytes = capture_bytes(tpl.with("station-7"));
    auto expected = wire_build([](JsonBuilder& b) {
        b.add("name", string_view("station-7"));
    });
    REQUIRE(bytes.size() == expected.size());
    CHECK(bytes == expected);
}

TEST_CASE("body_template: string slot from std::string") {
    constexpr auto tpl = body_template<R"({"name":$1s})">();
    std::string name = "sensor-alpha";
    auto bytes = capture_bytes(tpl.with(name));
    auto expected = wire_build([&](JsonBuilder& b) {
        b.add("name", string_view(name));
    });
    REQUIRE(bytes.size() == expected.size());
    CHECK(bytes == expected);
}

TEST_CASE("body_template: empty string slot") {
    constexpr auto tpl = body_template<R"({"k":$1s})">();
    auto bytes = capture_bytes(tpl.with(""));
    auto expected = wire_build([](JsonBuilder& b) {
        b.add("k", string_view(""));
    });
    REQUIRE(bytes.size() == expected.size());
    CHECK(bytes == expected);
}

TEST_CASE("body_template: multiple string slots") {
    constexpr auto tpl = body_template<R"({"a":$1s,"b":$2s})">();
    auto bytes = capture_bytes(tpl.with("hello", "world"));
    auto expected = wire_build([](JsonBuilder& b) {
        b.add("a", string_view("hello"));
        b.add("b", string_view("world"));
    });
    REQUIRE(bytes.size() == expected.size());
    CHECK(bytes == expected);
}

TEST_CASE("body_template: all four slot types mixed") {
    constexpr auto tpl = body_template<
        R"({"name":$1s,"seq":$2,"temp":$3f,"alarm":$4b})">();
    auto bytes = capture_bytes(tpl.with("device-A", int32_t{42}, 18.0, true));
    auto expected = wire_build([](JsonBuilder& b) {
        b.add("name", string_view("device-A"));
        b.add("seq", int32_t{42});
        b.add("temp", 18.0);
        b.add("alarm", true);
    });
    REQUIRE(bytes.size() == expected.size());
    CHECK(bytes == expected);
}

TEST_CASE("body_template: string slot lifetime — view holds caller's data") {
    // The body_template_call captures string_view by value; the
    // underlying chars must outlive the call. Here we use a std::string
    // local that outlives the capture_bytes() invocation.
    constexpr auto tpl = body_template<R"({"k":$1s})">();
    std::string longer = "this is a longer string than the literal in the template";
    auto bytes = capture_bytes(tpl.with(longer));
    auto expected = wire_build([&](JsonBuilder& b) {
        b.add("k", string_view(longer));
    });
    REQUIRE(bytes.size() == expected.size());
    CHECK(bytes == expected);
}

// ---------------------------------------------------------------------------
// BodyValue integration — `req.body(tpl.with(...))` path. Works in both wire
// modes via the active builder + `begin_raw_value` splice.
// ---------------------------------------------------------------------------

TEST_CASE("body_template: BodyValue conversion splices a \"body\" field") {
    constexpr auto tpl = body_template<R"({"seq":$1,"temp":$2f})">();
    BodyValue bv = tpl.with(int32_t{42}, 22.5);  // implicit conversion
    auto actual = body_value_bytes(bv);

    auto expected = wire_build([](JsonBuilder& b) {
        b.begin_object("body");
        b.add("seq", int32_t{42});
        b.add("temp", 22.5);
        b.end_object();
    });

    REQUIRE(actual.size() == expected.size());
    CHECK(actual == expected);
}

TEST_CASE("body_template: BodyValue conversion produces same bytes as note::body lambda") {
    constexpr auto tpl = body_template<R"({"x":$1,"y":$2,"on":$3b})">();

    auto tpl_bytes = body_value_bytes(tpl.with(int32_t{1}, int32_t{2}, true));
    auto lambda_bytes = body_value_bytes(body([](JsonBuilder& b) {
        b.add("x", int32_t{1});
        b.add("y", int32_t{2});
        b.add("on", true);
    }));

    CHECK(tpl_bytes == lambda_bytes);
}

TEST_CASE("body_template: BodyValue with string slot matches note::body lambda") {
    // The string-slot path through BodyValue is the headline use case —
    // sensor readings with a device name and runtime numeric values.
    constexpr auto tpl = body_template<
        R"({"name":$1s,"seq":$2,"temp":$3f})">();

    auto tpl_bytes = body_value_bytes(tpl.with("station-7", int32_t{17}, 22.5));
    auto lambda_bytes = body_value_bytes(body([](JsonBuilder& b) {
        b.add("name", string_view("station-7"));
        b.add("seq", int32_t{17});
        b.add("temp", 22.5);
    }));

    CHECK(tpl_bytes == lambda_bytes);
}

// ===========================================================================
// Surface 2: make_body<L>(args...) — one-call template literal + values.
// Equivalent to body_template<L>().with(args...). Sugar.
// ===========================================================================

TEST_CASE("make_body<L>: one-call form is equivalent to body_template+with") {
    auto from_make_body = capture_bytes(
        make_body<R"({"name":$1s,"seq":$2,"temp":$3f})">(
            "station-7", int32_t{42}, 22.5));

    constexpr auto tpl = body_template<
        R"({"name":$1s,"seq":$2,"temp":$3f})">();
    auto from_two_step = capture_bytes(tpl.with("station-7", int32_t{42}, 22.5));

    CHECK(from_make_body == from_two_step);
}

TEST_CASE("make_body<L>: empty object inline") {
    auto bytes = capture_bytes(make_body<R"({})">());
    auto expected = wire_build([](JsonBuilder&) {});
    CHECK(bytes == expected);
}

// ===========================================================================
// Surface 3: body_object{ "k"_k = v, ... } — init-list with UDL keys.
// Each `_k` UDL produces a key_tag; `operator=` pairs it with a value;
// CTAD on body_object{...} aggregates the field_pairs into a body.
// ===========================================================================

TEST_CASE("body_object: single int32 field via UDL") {
    using namespace note::body_literals;
    auto body = body_object{ "a"_k = 42 };
    auto bytes = capture_bytes(body);
    auto expected = wire_build([](JsonBuilder& b) {
        b.add("a", int32_t{42});
    });
    CHECK(bytes == expected);
}

TEST_CASE("body_object: int + double + bool + string mixed") {
    using namespace note::body_literals;
    auto body = body_object{
        "name"_k  = "station-7",
        "seq"_k   = 42,
        "temp"_k  = 22.5,
        "alarm"_k = true,
    };
    auto bytes = capture_bytes(body);
    auto expected = wire_build([](JsonBuilder& b) {
        b.add("name", string_view("station-7"));
        b.add("seq", int32_t{42});
        b.add("temp", 22.5);
        b.add("alarm", true);
    });
    CHECK(bytes == expected);
}

TEST_CASE("body_object: int slot inferred from non-int32 integral type") {
    using namespace note::body_literals;
    // short, char, etc. should all canonicalise to int32_t through key_tag::operator=
    short small = 7;
    auto body = body_object{ "v"_k = small };
    auto bytes = capture_bytes(body);
    auto expected = wire_build([](JsonBuilder& b) {
        b.add("v", int32_t{7});
    });
    CHECK(bytes == expected);
}

TEST_CASE("body_object: float widens to double") {
    using namespace note::body_literals;
    auto body = body_object{ "t"_k = 22.5f };
    auto bytes = capture_bytes(body);
    auto expected = wire_build([](JsonBuilder& b) {
        b.add("t", static_cast<double>(22.5f));
    });
    CHECK(bytes == expected);
}

TEST_CASE("body_object: bool strictly requires bool (no int implicit)") {
    using namespace note::body_literals;
    // This compiles — bool is bool.
    auto body = body_object{ "on"_k = true };
    auto bytes = capture_bytes(body);
    auto expected = wire_build([](JsonBuilder& b) { b.add("on", true); });
    CHECK(bytes == expected);

    // `"on"_k = 1` would resolve via the integral overload to slot_type::Int32,
    // not bool — confirmed by the compile-fail test suite.
}

TEST_CASE("body_object: std::string and string_view both work for string slot") {
    using namespace note::body_literals;
    std::string str_value = "hello";

    auto body1 = body_object{ "k"_k = str_value };
    auto body2 = body_object{ "k"_k = string_view(str_value) };

    auto bytes1 = capture_bytes(body1);
    auto bytes2 = capture_bytes(body2);
    CHECK(bytes1 == bytes2);
}

// ===========================================================================
// Surface 4: body_builder().add(...).add(...) — fluent type-state.
// Each .add returns a new builder type with the field appended; the chain
// converts implicitly to body_object and through that to BodyValue.
// ===========================================================================

TEST_CASE("body_builder: empty body") {
    auto bytes = capture_bytes(body_builder());
    auto expected = wire_build([](JsonBuilder&) {});
    CHECK(bytes == expected);
}

TEST_CASE("body_builder: single field") {
    using namespace note::body_literals;
    auto builder = body_builder().add("a"_k, 42);
    auto bytes = capture_bytes(builder);
    auto expected = wire_build([](JsonBuilder& b) { b.add("a", int32_t{42}); });
    CHECK(bytes == expected);
}

TEST_CASE("body_builder: fluent chain produces same bytes as init-list") {
    using namespace note::body_literals;
    auto builder = body_builder()
        .add("name"_k,  "station-7")
        .add("seq"_k,   42)
        .add("temp"_k,  22.5)
        .add("alarm"_k, true);
    auto chain_bytes = capture_bytes(builder);

    auto init_body = body_object{
        "name"_k  = "station-7",
        "seq"_k   = 42,
        "temp"_k  = 22.5,
        "alarm"_k = true,
    };
    auto init_bytes = capture_bytes(init_body);

    CHECK(chain_bytes == init_bytes);
}

// ===========================================================================
// Cross-surface parity — same logical body via different surfaces produces
// the same wire bytes.
// ===========================================================================

TEST_CASE("cross-surface parity: body_template / jsonb / body_object / body_builder all agree") {
    using namespace note::body_literals;
    // Logical body: {"name": "station-7", "seq": 42, "temp": 22.5}

    auto from_body_template = capture_bytes(
        body_template<R"({"name":$1s,"seq":$2,"temp":$3f})">()
            .with("station-7", 42, 22.5));

    auto from_make_body = capture_bytes(
        make_body<R"({"name":$1s,"seq":$2,"temp":$3f})">(
            "station-7", 42, 22.5));

    auto from_body_object = capture_bytes(body_object{
        "name"_k = "station-7",
        "seq"_k  = 42,
        "temp"_k = 22.5,
    });

    auto from_builder = capture_bytes(
        body_builder()
            .add("name"_k, "station-7")
            .add("seq"_k,  42)
            .add("temp"_k, 22.5)
    );

    auto from_lambda = wire_build([](JsonBuilder& b) {
        b.add("name", string_view("station-7"));
        b.add("seq", int32_t{42});
        b.add("temp", 22.5);
    });

    CHECK(from_body_template == from_lambda);
    CHECK(from_make_body         == from_lambda);
    CHECK(from_body_object    == from_lambda);
    CHECK(from_builder       == from_lambda);
}

// ===========================================================================
// Wire-format literal checks — independent of the StreamingJson*Builder
// reference, so a shared bug between impl and reference can't hide. One per
// build mode asserts the exact bytes the body_template emits.
// ===========================================================================

#if NOTE_JSONB != 1
namespace {
std::string as_string(const std::vector<uint8_t>& v) {
    return std::string(v.begin(), v.end());
}
}  // namespace

TEST_CASE("body_template: emits exact JSON text (all slot types + nesting)") {
    using namespace note::body_literals;
    auto body = body_object{
        "name"_k  = "station-7",
        "seq"_k   = 42,
        "temp"_k  = 22.5,
        "on"_k    = true,
        "loc"_k   = body_object{ "lat"_k = 1 },
        "tags"_k  = body_array{"a", "b"},
    };
    CHECK(as_string(capture_bytes(body)) ==
        R"({"name":"station-7","seq":42,"temp":22.5,"on":true,"loc":{"lat":1},"tags":["a","b"]})");
}

TEST_CASE("body_template: JSON string slot escapes quotes and backslashes") {
    using namespace note::body_literals;
    auto body = body_object{ "k"_k = R"(a"b\c)" };
    CHECK(as_string(capture_bytes(body)) == R"({"k":"a\"b\\c"})");
}

TEST_CASE("body_template: BodyValue emits exact \"body\":{...} JSON text") {
    using namespace note::body_literals;
    auto body = body_object{ "seq"_k = 7 };
    // body_value_bytes wraps in a fresh request object: {"body":{...}}.
    CHECK(as_string(body_value_bytes(body)) == R"({"body":{"seq":7}})");
}
#endif  // NOTE_JSONB != 1

// ===========================================================================
// Stage 5a — nesting + arrays for surfaces 3 (body_object) and 4 (body_builder).
//
//   "k"_k = body_object{...}   nests an object as a field value.
//   "k"_k = body_array{...}  nests an array as a field value.
//   body_array{a, b, c}      a free-standing heterogeneous array.
//
// Nested values carry their own emit_to(); the outer schema bakes only the
// key bytes (no opcode prefix, no value bytes) and dispatches to the nested
// value at the slot position.
// ===========================================================================

TEST_CASE("body_array: homogeneous string elements as a field value") {
    using namespace note::body_literals;
    auto body = body_object{
        "tags"_k = body_array{"red", "green", "blue"},
    };
    auto bytes = capture_bytes(body);
    auto expected = wire_build([](JsonBuilder& b) {
        b.begin_array("tags");
        b.add_element(string_view("red"));
        b.add_element(string_view("green"));
        b.add_element(string_view("blue"));
        b.end_array();
    });
    REQUIRE(bytes.size() == expected.size());
    CHECK(bytes == expected);
}

TEST_CASE("body_array: heterogeneous elements as a field value") {
    using namespace note::body_literals;
    auto body = body_object{
        "vals"_k = body_array{42, "text", 22.5, true},
    };
    auto bytes = capture_bytes(body);
    auto expected = wire_build([](JsonBuilder& b) {
        b.begin_array("vals");
        b.add_element(int32_t{42});
        b.add_element(string_view("text"));
        b.add_element(22.5);
        b.add_element(true);
        b.end_array();
    });
    REQUIRE(bytes.size() == expected.size());
    CHECK(bytes == expected);
}

TEST_CASE("body_array: empty array as a field value") {
    using namespace note::body_literals;
    auto body = body_object{
        "tags"_k = body_array{},
    };
    auto bytes = capture_bytes(body);
    auto expected = wire_build([](JsonBuilder& b) {
        b.begin_array("tags");
        b.end_array();
    });
    REQUIRE(bytes.size() == expected.size());
    CHECK(bytes == expected);
}

TEST_CASE("body_object: nested body as a field value") {
    using namespace note::body_literals;
    auto body = body_object{
        "name"_k   = "outer",
        "nested"_k = body_object{ "x"_k = 1, "y"_k = 2 },
    };
    auto bytes = capture_bytes(body);
    auto expected = wire_build([](JsonBuilder& b) {
        b.add("name", string_view("outer"));
        b.begin_object("nested");
        b.add("x", int32_t{1});
        b.add("y", int32_t{2});
        b.end_object();
    });
    REQUIRE(bytes.size() == expected.size());
    CHECK(bytes == expected);
}

TEST_CASE("body_object: deeply nested object (body in body in body)") {
    using namespace note::body_literals;
    auto body = body_object{
        "a"_k = body_object{
            "b"_k = body_object{
                "c"_k = 99,
            },
        },
    };
    auto bytes = capture_bytes(body);
    auto expected = wire_build([](JsonBuilder& b) {
        b.begin_object("a");
        b.begin_object("b");
        b.add("c", int32_t{99});
        b.end_object();
        b.end_object();
    });
    REQUIRE(bytes.size() == expected.size());
    CHECK(bytes == expected);
}

TEST_CASE("body_object: array of nested objects") {
    using namespace note::body_literals;
    auto body = body_object{
        "items"_k = body_array{
            body_object{ "id"_k = 1 },
            body_object{ "id"_k = 2 },
        },
    };
    auto bytes = capture_bytes(body);
    auto expected = wire_build([](JsonBuilder& b) {
        b.begin_array("items");
        b.begin_element_object();
        b.add("id", int32_t{1});
        b.end_object();
        b.begin_element_object();
        b.add("id", int32_t{2});
        b.end_object();
        b.end_array();
    });
    REQUIRE(bytes.size() == expected.size());
    CHECK(bytes == expected);
}

TEST_CASE("body_object: mix of primitive and nested fields") {
    using namespace note::body_literals;
    auto body = body_object{
        "name"_k  = "station-7",
        "loc"_k   = body_object{ "lat"_k = 1.5, "lon"_k = 2.5 },
        "tags"_k  = body_array{"a", "b"},
        "seq"_k   = 42,
    };
    auto bytes = capture_bytes(body);
    auto expected = wire_build([](JsonBuilder& b) {
        b.add("name", string_view("station-7"));
        b.begin_object("loc");
        b.add("lat", 1.5);
        b.add("lon", 2.5);
        b.end_object();
        b.begin_array("tags");
        b.add_element(string_view("a"));
        b.add_element(string_view("b"));
        b.end_array();
        b.add("seq", int32_t{42});
    });
    REQUIRE(bytes.size() == expected.size());
    CHECK(bytes == expected);
}

TEST_CASE("body_builder: nested body via .add") {
    using namespace note::body_literals;
    auto builder = body_builder()
        .add("name"_k,   "outer")
        .add("nested"_k, body_object{ "x"_k = 1 });
    auto bytes = capture_bytes(builder);
    auto expected = wire_build([](JsonBuilder& b) {
        b.add("name", string_view("outer"));
        b.begin_object("nested");
        b.add("x", int32_t{1});
        b.end_object();
    });
    REQUIRE(bytes.size() == expected.size());
    CHECK(bytes == expected);
}

TEST_CASE("body_builder: nested array via .add matches init-list") {
    using namespace note::body_literals;
    auto from_builder = capture_bytes(
        body_builder()
            .add("tags"_k, body_array{"a", "b", "c"}));
    auto from_init = capture_bytes(body_object{
        "tags"_k = body_array{"a", "b", "c"},
    });
    CHECK(from_builder == from_init);
}

// ===========================================================================
// Stage 5b — nesting + arrays for the template literal (surfaces 1 + 2).
//
//   $No  — object slot: the positional arg must be a body_object (or array).
//   $Na  — array slot:  the positional arg must be a body_array (or body).
//
// Both map to slot_type::Nested; the o/a suffix documents intent. The
// argument carries its own emit_to(), so the template's static pool holds
// only the field key (no opcode prefix, no value bytes).
// ===========================================================================

TEST_CASE("body_template: object slot $No") {
    using namespace note::body_literals;
    constexpr auto tpl = body_template<R"({"loc":$1o})">();
    auto bytes = capture_bytes(
        tpl.with(body_object{ "lat"_k = 1.5, "lon"_k = 2.5 }));
    auto expected = wire_build([](JsonBuilder& b) {
        b.begin_object("loc");
        b.add("lat", 1.5);
        b.add("lon", 2.5);
        b.end_object();
    });
    REQUIRE(bytes.size() == expected.size());
    CHECK(bytes == expected);
}

TEST_CASE("body_template: array slot $Na") {
    using namespace note::body_literals;
    constexpr auto tpl = body_template<R"({"tags":$1a})">();
    auto bytes = capture_bytes(
        tpl.with(body_array{"red", "green", "blue"}));
    auto expected = wire_build([](JsonBuilder& b) {
        b.begin_array("tags");
        b.add_element(string_view("red"));
        b.add_element(string_view("green"));
        b.add_element(string_view("blue"));
        b.end_array();
    });
    REQUIRE(bytes.size() == expected.size());
    CHECK(bytes == expected);
}

TEST_CASE("body_template: object + array + primitive slots mixed") {
    using namespace note::body_literals;
    constexpr auto tpl = body_template<
        R"({"name":$1s,"loc":$2o,"items":$3a,"seq":$4})">();
    auto bytes = capture_bytes(tpl.with(
        "station-7",
        body_object{ "lat"_k = 1.5 },
        body_array{1, 2, 3},
        int32_t{42}));
    auto expected = wire_build([](JsonBuilder& b) {
        b.add("name", string_view("station-7"));
        b.begin_object("loc");
        b.add("lat", 1.5);
        b.end_object();
        b.begin_array("items");
        b.add_element(int32_t{1});
        b.add_element(int32_t{2});
        b.add_element(int32_t{3});
        b.end_array();
        b.add("seq", int32_t{42});
    });
    REQUIRE(bytes.size() == expected.size());
    CHECK(bytes == expected);
}

TEST_CASE("body_template: $No / $Na agree with body_object nested fields") {
    using namespace note::body_literals;
    auto from_template = capture_bytes(
        body_template<R"({"outer":$1o,"items":$2a})">()
            .with(body_object{ "x"_k = 1 },
                  body_array{1, 2, 3}));
    auto from_init = capture_bytes(body_object{
        "outer"_k = body_object{ "x"_k = 1 },
        "items"_k = body_array{1, 2, 3},
    });
    CHECK(from_template == from_init);
}

TEST_CASE("make_body<L>: nested object/array slots via one-call form") {
    using namespace note::body_literals;
    auto from_make_body = capture_bytes(
        make_body<R"({"loc":$1o})">(
            body_object{ "lat"_k = 1.5 }));
    auto from_two_step = capture_bytes(
        body_template<R"({"loc":$1o})">()
            .with(body_object{ "lat"_k = 1.5 }));
    CHECK(from_make_body == from_two_step);
}

// ===========================================================================
// BodyValue integration for the new surfaces — `req.body(...)` path. Works in
// both wire modes (shared `compiled_body` base + `begin_raw_value` splice).
// ===========================================================================

TEST_CASE("body_object: BodyValue conversion splices a \"body\" field") {
    using namespace note::body_literals;
    auto body = body_object{
        "seq"_k  = 42,
        "temp"_k = 22.5,
    };
    auto actual = body_value_bytes(body);

    auto expected = wire_build([](JsonBuilder& b) {
        b.begin_object("body");
        b.add("seq", int32_t{42});
        b.add("temp", 22.5);
        b.end_object();
    });

    CHECK(actual == expected);
}

TEST_CASE("body_builder: BodyValue conversion (named-builder lifetime)") {
    // Builder must outlive the BodyValue — same contract as note::body(lambda).
    // Here the builder lives in this scope; bv captures &builder.
    using namespace note::body_literals;
    auto builder = body_builder()
        .add("x"_k, 1)
        .add("y"_k, 2);
    auto actual = body_value_bytes(builder);

    auto expected = wire_build([](JsonBuilder& b) {
        b.begin_object("body");
        b.add("x", int32_t{1});
        b.add("y", int32_t{2});
        b.end_object();
    });

    CHECK(actual == expected);
}

// A nested body/array survives the BodyValue splice in both wire formats.
TEST_CASE("body_object: BodyValue conversion with nested object + array") {
    using namespace note::body_literals;
    auto body = body_object{
        "name"_k = "station-7",
        "loc"_k  = body_object{ "lat"_k = 1.5, "lon"_k = 2.5 },
        "tags"_k = body_array{"a", "b"},
    };
    auto actual = body_value_bytes(body);

    auto expected = wire_build([](JsonBuilder& b) {
        b.begin_object("body");
        b.add("name", string_view("station-7"));
        b.begin_object("loc");
        b.add("lat", 1.5);
        b.add("lon", 2.5);
        b.end_object();
        b.begin_array("tags");
        b.add_element(string_view("a"));
        b.add_element(string_view("b"));
        b.end_array();
        b.end_object();
    });

    CHECK(actual == expected);
}

