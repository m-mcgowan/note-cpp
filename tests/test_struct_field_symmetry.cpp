// Field-type symmetry test: every supported schema-struct field type
// should round-trip identically through serialisation, streaming
// deserialisation, random-access deserialisation, and template
// registration. One TEST_CASE per field shape exercises all four paths
// so asymmetries fail loudly instead of drifting.
//
// Types covered:
//   bool, int8/16/32/64, float, double, char[N], string_view,
//   std::string, FakeArduinoString (host shim for Arduino String),
//   nested NOTE_FIELDS struct, nested reflectable aggregate,
//   std::array of primitives.

#include <doctest.h>
#include "test_json_backend.hpp"
#include "test_notecard_factory.hpp"

#include <note/allocator.hpp>
#include <note/arena.hpp>
#include <note/body.hpp>
#include <note/json_sax.hpp>
#include <note/json_sax_streaming.hpp>
#include <note/notecard.hpp>
#include <note/string_pool.hpp>
#include <note/struct_sink.hpp>
#include <note/api.hpp>

#include <array>
#include <cstring>
#include <memory>
#include <string>

namespace {

// ── Test harness shared across field-type cases ─────────────────────────────

struct TestRequest {
    static constexpr note::string_view notecard_request = "test.req";
    [[maybe_unused]] static constexpr bool supports_cmd = false;
    [[maybe_unused]] static constexpr note::Safety safety = note::Safety::Idempotent;

    note::BodyValue body{};

    struct Response {
        static Response parse(std::unique_ptr<note::JsonReader>) { return {}; }
        static Response parse(const note::JsonReader&) { return {}; }
    };

    void build(note::JsonBuilder& b) const { body.write_to(b); }
};

struct Harness {
    note::test::TestJsonBackend backend;
    std::string last_request;
    std::string last_response{"{}"};
    note::test::CallbackTransport transport;
    std::unique_ptr<note::Notecard> nc_ptr;
    note::Notecard& nc;

    Harness()
        : transport(
            [this](note::string_view req, uint32_t) -> note::Result<note::string_view> {
                last_request = std::string(req);
                last_response = "{}";
                return note::string_view(last_response);
            },
            [this](note::string_view req) -> note::Result<void> {
                last_request = std::string(req);
                return {};
            })
        , nc_ptr(note::test::make_test_notecard_heap(backend, transport))
        , nc(*nc_ptr) {}

    // Serialise `s` as a request body; return just the "body":{...} JSON
    // fragment (the TestRequest wrapper is `{"req":"test.req","body":{...}}`).
    template<typename T>
    std::string ser_body(const T& s) {
        TestRequest req;
        req.body = note::make_schema_body(s);
        nc.execute(req);
        constexpr const char* prefix = R"({"req":"test.req",)";
        const std::size_t pre = std::strlen(prefix);
        REQUIRE(last_request.substr(0, pre) == prefix);
        // Strip wrapping object braces and the request prefix.
        // Result: body":{...}}   →   keep up to final } then drop it.
        std::string rest = last_request.substr(pre);
        REQUIRE(rest.back() == '}');
        rest.pop_back();  // drop the outermost closing brace
        return rest;       // "body":{...}
    }

    template<typename T>
    std::string template_body() {
        TestRequest req;
        req.body = note::template_of<T>();
        nc.execute(req);
        constexpr const char* prefix = R"({"req":"test.req",)";
        const std::size_t pre = std::strlen(prefix);
        std::string rest = last_request.substr(pre);
        rest.pop_back();
        return rest;
    }
};

// Streaming deser: feed a JSON body fragment to StructSink<T> and return
// the parsed struct. `body_json` is expected to look like
// `{"field":value,...}` — we drive the sink events directly to bypass
// the transport layer.
//
// For clarity, tests construct a JSON fragment, parse it via a SAX
// driver routed into StructSink. Here we short-circuit by directly
// invoking sink events that match the test fragment.

// Simpler: use note::sax_parse to feed the events for us.

// ── Field types under test ──────────────────────────────────────────────────

struct BoolOnly {
    bool flag;
    NOTE_FIELDS(flag)
    bool operator==(const BoolOnly& o) const { return flag == o.flag; }
};

struct Int8Only {
    std::int8_t value;
    NOTE_FIELDS(value)
    bool operator==(const Int8Only& o) const { return value == o.value; }
};

struct Int16Only {
    std::int16_t value;
    NOTE_FIELDS(value)
    bool operator==(const Int16Only& o) const { return value == o.value; }
};

struct Int32Only {
    std::int32_t value;
    NOTE_FIELDS(value)
    bool operator==(const Int32Only& o) const { return value == o.value; }
};

struct Int64Only {
    std::int64_t value;
    NOTE_FIELDS(value)
    bool operator==(const Int64Only& o) const { return value == o.value; }
};

struct FloatOnly {
    float value;
    NOTE_FIELDS(value)
    bool operator==(const FloatOnly& o) const { return value == o.value; }
};

struct DoubleOnly {
    double value;
    NOTE_FIELDS(value)
    bool operator==(const DoubleOnly& o) const { return value == o.value; }
};

struct CharArray8 {
    char tag[8];
    NOTE_FIELDS(tag)
    bool operator==(const CharArray8& o) const {
        return std::strncmp(tag, o.tag, 8) == 0;
    }
};

struct StringViewOnly {
    note::string_view sv;
    NOTE_FIELDS(sv)
    bool operator==(const StringViewOnly& o) const { return sv == o.sv; }
};

struct StdStringOnly {
    std::string text;
    NOTE_FIELDS(text)
    bool operator==(const StdStringOnly& o) const { return text == o.text; }
};

// Host-side shim for Arduino String: (const char*) and (const char*, size_t)
// ctors + .c_str(). Exercises the has_c_str_v path in ser.
struct FakeArduinoString {
    std::string buf_;
    FakeArduinoString() = default;
    FakeArduinoString(const char* p) : buf_(p ? p : "") {}
    FakeArduinoString(const char* p, std::size_t n) : buf_(p, n) {}
    const char* c_str() const { return buf_.c_str(); }
    bool operator==(const FakeArduinoString& o) const { return buf_ == o.buf_; }
};

struct ArduinoStringLike {
    FakeArduinoString name;
    NOTE_FIELDS(name)
    bool operator==(const ArduinoStringLike& o) const { return name == o.name; }
};

// Nested NOTE_FIELDS struct.
struct NestedFields {
    float x;
    int16_t y;
    NOTE_FIELDS(x, y)
    bool operator==(const NestedFields& o) const {
        return x == o.x && y == o.y;
    }
};

struct WithNestedFields {
    NestedFields nested;
    NOTE_FIELDS(nested)
    bool operator==(const WithNestedFields& o) const { return nested == o.nested; }
};

// std::array of primitives.
struct IntArrayField {
    std::array<int32_t, 3> ints;
    NOTE_FIELDS(ints)
    bool operator==(const IntArrayField& o) const { return ints == o.ints; }
};

// std::array of NOTE_FIELDS structs — exercises begin_element_object path.
struct StructArrayField {
    std::array<NestedFields, 2> items;
    NOTE_FIELDS(items)
    bool operator==(const StructArrayField& o) const { return items == o.items; }
};

// std::array of char[N] — fixed-length string elements.
struct CharArrayArrayField {
    std::array<char[4], 2> tags;
    NOTE_FIELDS(tags)
    bool operator==(const CharArrayArrayField& o) const {
        for (std::size_t i = 0; i < 2; ++i)
            if (std::strncmp(tags[i], o.tags[i], 4) != 0) return false;
        return true;
    }
};

// ── Helpers ─────────────────────────────────────────────────────────────────

// Adapter that forwards SAX events into a StructSink after skipping the
// outermost { ... } pair. StructSink treats its event stream as "already
// inside the object" — the wrapping obj_begin/obj_end would otherwise
// steer it into skip mode.
template<typename T>
struct RootStrippingSink {
    note::StructSink<T>& inner;
    int depth = 0;

    void on_null(note::string_view k) { if (depth > 0) inner.on_null(k); }
    void on_bool(note::string_view k, bool v) { if (depth > 0) inner.on_bool(k, v); }
    void on_int(note::string_view k, note::json_int_t v) { if (depth > 0) inner.on_int(k, v); }
    void on_float(note::string_view k, double v) { if (depth > 0) inner.on_float(k, v); }
    void on_string(note::string_view k, note::string_view v) { if (depth > 0) inner.on_string(k, v); }
    void on_number(note::string_view k, note::string_view v) { if (depth > 0) inner.on_number(k, v); }
    void on_object_begin(note::string_view k) {
        if (depth > 0) inner.on_object_begin(k);
        ++depth;
    }
    void on_object_end(note::string_view k) {
        --depth;
        if (depth > 0) inner.on_object_end(k);
    }
    void on_array_begin(note::string_view k) { if (depth > 0) inner.on_array_begin(k); }
    void on_array_end(note::string_view k) { if (depth > 0) inner.on_array_end(k); }
    void reset() { inner.reset(); }
};

template<typename T>
T roundtrip_via_struct_sink(std::string body_fragment) {
    const std::string prefix = R"("body":)";
    REQUIRE(body_fragment.substr(0, prefix.size()) == prefix);
    std::string obj_json = body_fragment.substr(prefix.size());

    char pool_buf[1024];
    note::MonotonicArena arena(pool_buf);
    note::StringPool pool(note::arena_allocator(arena));
    T out{};
    note::StructSink<T> sink(out, pool);
    RootStrippingSink<T> adapter{sink, 0};

    std::size_t pos = 0;
    auto read = [&](uint8_t* b, std::size_t max, uint32_t) -> note::Result<std::size_t> {
        std::size_t n = obj_json.size() - pos;
        if (n > max) n = max;
        for (std::size_t i = 0; i < n; ++i) b[i] = static_cast<uint8_t>(obj_json[pos + i]);
        pos += n;
        return n;
    };
    auto err = note::sax_parse_streaming(read, 1000, adapter);
    REQUIRE(err.empty());
    return out;
}

// Round-trip helper — runs ser through the harness, then parses via
// streaming deser and returns the restored struct.
template<typename T>
T roundtrip(Harness& h, const T& original) {
    std::string body = h.ser_body(original);
    return roundtrip_via_struct_sink<T>(body);
}

} // namespace

// ── Primitive field types ───────────────────────────────────────────────────

TEST_CASE("symmetry: bool field") {
    Harness h;
    BoolOnly s{true};
    REQUIRE(h.ser_body(s) == R"("body":{"flag":true})");
    REQUIRE(h.template_body<BoolOnly>() == R"("body":{"flag":true})");
    REQUIRE(roundtrip(h, s) == s);
}

TEST_CASE("symmetry: int8_t field") {
    Harness h;
    Int8Only s{-42};
    REQUIRE(h.ser_body(s) == R"("body":{"value":-42})");
    REQUIRE(h.template_body<Int8Only>() == R"("body":{"value":1})");
    REQUIRE(roundtrip(h, s) == s);
}

TEST_CASE("symmetry: int16_t field") {
    Harness h;
    Int16Only s{-1000};
    REQUIRE(h.ser_body(s) == R"("body":{"value":-1000})");
    REQUIRE(h.template_body<Int16Only>() == R"("body":{"value":11})");
    REQUIRE(roundtrip(h, s) == s);
}

TEST_CASE("symmetry: int32_t field") {
    Harness h;
    Int32Only s{100000};
    REQUIRE(h.ser_body(s) == R"("body":{"value":100000})");
    REQUIRE(h.template_body<Int32Only>() == R"("body":{"value":12})");
    REQUIRE(roundtrip(h, s) == s);
}

TEST_CASE("symmetry: int64_t field") {
    Harness h;
    Int64Only s{1LL << 33};
    REQUIRE(h.ser_body(s) == R"("body":{"value":8589934592})");
    REQUIRE(h.template_body<Int64Only>() == R"("body":{"value":12})");
    REQUIRE(roundtrip(h, s) == s);
}

TEST_CASE("symmetry: float field") {
    Harness h;
    FloatOnly s{3.25f};
    REQUIRE(h.ser_body(s) == R"("body":{"value":3.25})");
    REQUIRE(h.template_body<FloatOnly>() == R"("body":{"value":14.1})");
    REQUIRE(roundtrip(h, s) == s);
}

TEST_CASE("symmetry: double field") {
    Harness h;
    DoubleOnly s{2.5};
    REQUIRE(h.ser_body(s) == R"("body":{"value":2.5})");
    REQUIRE(h.template_body<DoubleOnly>() == R"("body":{"value":14.1})");
    REQUIRE(roundtrip(h, s) == s);
}

// ── String-like field types ─────────────────────────────────────────────────

TEST_CASE("symmetry: char[N] field emits TSTRING(N) template hint") {
    Harness h;
    CharArray8 s{};
    std::memcpy(s.tag, "hello", 6);
    REQUIRE(h.ser_body(s) == R"("body":{"tag":"hello"})");
    // Regression: pre-fix this was "1" (TSTRING(1)); now the Notecard sees
    // the correct max length of 8.
    REQUIRE(h.template_body<CharArray8>() == R"("body":{"tag":"xxxxxxxx"})");
    REQUIRE(roundtrip(h, s) == s);
}

TEST_CASE("symmetry: string_view field") {
    Harness h;
    StringViewOnly s{"view"};
    REQUIRE(h.ser_body(s) == R"("body":{"sv":"view"})");
    REQUIRE(h.template_body<StringViewOnly>() == R"("body":{"sv":"1"})");
    REQUIRE(roundtrip(h, s) == s);
}

TEST_CASE("symmetry: std::string field") {
    Harness h;
    StdStringOnly s{"std-value"};
    REQUIRE(h.ser_body(s) == R"("body":{"text":"std-value"})");
    REQUIRE(h.template_body<StdStringOnly>() == R"("body":{"text":"1"})");
    REQUIRE(roundtrip(h, s) == s);
}

TEST_CASE("symmetry: Arduino-String-like field (c_str ser, ptr-len deser)") {
    Harness h;
    ArduinoStringLike s{};
    s.name = FakeArduinoString("arduino");
    REQUIRE(h.ser_body(s) == R"("body":{"name":"arduino"})");
    REQUIRE(h.template_body<ArduinoStringLike>() == R"("body":{"name":"1"})");
    REQUIRE(roundtrip(h, s) == s);
}

// ── Nested aggregate ────────────────────────────────────────────────────────

TEST_CASE("symmetry: nested NOTE_FIELDS struct") {
    Harness h;
    WithNestedFields s{{1.5f, 42}};
    REQUIRE(h.ser_body(s) ==
        R"("body":{"nested":{"x":1.5,"y":42}})");
    if constexpr (note::detail::notecard_supports_nested_templates_v) {
        REQUIRE(h.template_body<WithNestedFields>() ==
            R"("body":{"nested":{"x":14.1,"y":11}})");
    }
    REQUIRE(roundtrip(h, s) == s);
}

// ── Array of primitives ─────────────────────────────────────────────────────

TEST_CASE("symmetry: std::array<int32_t,3> field") {
    Harness h;
    IntArrayField s{{1, 2, 3}};
    REQUIRE(h.ser_body(s) == R"("body":{"ints":[1,2,3]})");
    // Template: Notecard convention is a single-element hint array
    // describing the per-element type.
    REQUIRE(h.template_body<IntArrayField>() == R"("body":{"ints":[12]})");
    REQUIRE(roundtrip(h, s) == s);
}

TEST_CASE("symmetry: std::array<NestedFields,2> field") {
    Harness h;
    StructArrayField s{{{{1.0f, 10}, {2.0f, 20}}}};
    REQUIRE(h.ser_body(s) ==
        R"("body":{"items":[{"x":1,"y":10},{"x":2,"y":20}]})");
    if constexpr (note::detail::notecard_supports_nested_templates_v) {
        REQUIRE(h.template_body<StructArrayField>() ==
            R"("body":{"items":[{"x":14.1,"y":11}]})");
    }
    REQUIRE(roundtrip(h, s) == s);
}

TEST_CASE("symmetry: std::array<char[4],2> field") {
    Harness h;
    CharArrayArrayField s{};
    std::memcpy(s.tags[0], "abc", 4);
    std::memcpy(s.tags[1], "xyz", 4);
    REQUIRE(h.ser_body(s) == R"("body":{"tags":["abc","xyz"]})");
    // Template: one hint element of length N=4 (TSTRING(4)).
    REQUIRE(h.template_body<CharArrayArrayField>() ==
        R"("body":{"tags":["xxxx"]})");
    REQUIRE(roundtrip(h, s) == s);
}
