// Tests for JsonBuf — constexpr-capable JSON buffer builder.
#include "catch.hpp"

#include <note/json_buf.hpp>

#include <string_view>
using namespace std::string_view_literals;

// ── Compile-time tests (static_assert) ──────────────────────────────────────

constexpr auto simple_request = [] {
    note::JsonBuf<128> b;
    b.add("req", "hub.set");
    b.add("mode", "periodic");
    b.close();
    return b;
}();

static_assert(simple_request);
static_assert(simple_request.view() == R"({"req":"hub.set","mode":"periodic"})");

constexpr auto with_int = [] {
    note::JsonBuf<64> b;
    b.add("req", "hub.set");
    b.add("outbound", 60);
    b.close();
    return b;
}();

static_assert(with_int.view() == R"({"req":"hub.set","outbound":60})");

constexpr auto with_bool = [] {
    note::JsonBuf<64> b;
    b.add("req", "card.binary");
    b.add("delete", true);
    b.close();
    return b;
}();

static_assert(with_bool.view() == R"({"req":"card.binary","delete":true})");

constexpr auto with_negative = [] {
    note::JsonBuf<64> b;
    b.add("offset", -42);
    b.close();
    return b;
}();

static_assert(with_negative.view() == R"({"offset":-42})");

constexpr auto nested_object = [] {
    note::JsonBuf<256> b;
    b.add("req", "note.add");
    b.add("file", "sensors.qo");
    b.begin_object("body");
        b.add("temperature", 22.5);
        b.add("humidity", 60);
        b.add("label", "room-42");
    b.end_object();
    b.close();
    return b;
}();

static_assert(nested_object);
static_assert(nested_object.view() ==
    R"({"req":"note.add","file":"sensors.qo","body":{"temperature":22.5,"humidity":60,"label":"room-42"}})");

constexpr auto with_array = [] {
    note::JsonBuf<128> b;
    b.add("req", "card.attn");
    b.begin_array("files");
        b.add("sensors.qo");
        b.add("config.db");
    b.end_array();
    b.close();
    return b;
}();

static_assert(with_array.view() == R"({"req":"card.attn","files":["sensors.qo","config.db"]})");

constexpr auto deeply_nested = [] {
    note::JsonBuf<256> b;
    b.add("req", "note.add");
    b.begin_object("body");
        b.add("sensor", "bme280");
        b.begin_object("readings");
            b.add("temp", 22.5);
            b.add("pressure", 1013);
        b.end_object();
    b.end_object();
    b.close();
    return b;
}();

static_assert(deeply_nested.view() ==
    R"({"req":"note.add","body":{"sensor":"bme280","readings":{"temp":22.5,"pressure":1013}}})");

constexpr auto escape_test = [] {
    note::JsonBuf<128> b;
    b.add("msg", "hello \"world\"\nline2");
    b.close();
    return b;
}();

static_assert(escape_test.view() == R"({"msg":"hello \"world\"\nline2"})");

// ── Compile-time: composable fragments ──────────────────────────────────────

constexpr auto composed_object = [] {
    auto body = note::JsonBuf<64>::object();
    body.add("temp", 22.5);
    body.add("humidity", 60);
    body.close();

    note::JsonBuf<256> b;
    b.add("req", "note.add");
    b.add("file", "sensors.qo");
    b.add("body", body);
    b.close();
    return b;
}();

static_assert(composed_object);
static_assert(composed_object.view() ==
    R"({"req":"note.add","file":"sensors.qo","body":{"temp":22.5,"humidity":60}})");

constexpr auto composed_array = [] {
    auto files = note::JsonBuf<64>::array();
    files.add("sensors.qo");
    files.add("config.db");
    files.close();

    note::JsonBuf<128> b;
    b.add("req", "card.attn");
    b.add("files", files);
    b.close();
    return b;
}();

static_assert(composed_array.view() ==
    R"({"req":"card.attn","files":["sensors.qo","config.db"]})");

constexpr auto array_of_objects = [] {
    auto r1 = note::JsonBuf<32>::object();
    r1.add("x", 1);
    r1.close();

    auto r2 = note::JsonBuf<32>::object();
    r2.add("x", 2);
    r2.close();

    auto arr = note::JsonBuf<128>::array();
    arr.add(r1);
    arr.add(r2);
    arr.close();

    note::JsonBuf<256> b;
    b.add("items", arr);
    b.close();
    return b;
}();

static_assert(array_of_objects.view() == R"({"items":[{"x":1},{"x":2}]})");

// ── Compile-time: json_const enforced ───────────────────────────────────────

constexpr auto enforced = note::json_const([] {
    note::JsonBuf<64> b;
    b.add("req", "hub.sync");
    b.close();
    return b;
});

static_assert(enforced.view() == R"({"req":"hub.sync"})");

// ── Compile-time: auto-sized json<>() ────────────────────────────────────────

constexpr auto auto_simple = note::json<[](auto& b) {
    b.add("req", "hub.set");
    b.add("mode", "periodic");
    b.close();
}>();

static_assert(auto_simple.view() == R"({"req":"hub.set","mode":"periodic"})");
// Buffer is exactly the size needed (+1 for null terminator).
static_assert(auto_simple.capacity() == auto_simple.size() + 1);
static_assert(!auto_simple.overflow());

constexpr auto auto_nested = note::json<[](auto& b) {
    b.add("req", "note.add");
    b.add("file", "sensors.qo");
    b.begin_object("body");
        b.add("temperature", 22.5);
        b.add("humidity", 60);
    b.end_object();
    b.close();
}>();

static_assert(auto_nested.view() ==
    R"({"req":"note.add","file":"sensors.qo","body":{"temperature":22.5,"humidity":60}})");
static_assert(auto_nested.capacity() == auto_nested.size() + 1);

constexpr auto auto_array = note::json<[](auto& b) {
    b.add("req", "card.attn");
    b.begin_array("files");
        b.add("sensors.qo");
        b.add("config.db");
    b.end_array();
    b.close();
}>();

static_assert(auto_array.view() == R"({"req":"card.attn","files":["sensors.qo","config.db"]})");
static_assert(auto_array.capacity() == auto_array.size() + 1);

// ── Compile-time: standalone array ──────────────────────────────────────────

constexpr auto standalone_array = [] {
    auto a = note::JsonBuf<64>::array();
    a.add(1);
    a.add(2);
    a.add(3);
    a.close();
    return a;
}();

static_assert(standalone_array.view() == "[1,2,3]");

// ── Runtime tests ───────────────────────────────────────────────────────────

TEST_CASE("JsonBuf simple request at runtime") {
    note::JsonBuf<128> b;
    b.add("req", "hub.set");
    b.add("mode", "periodic");
    b.close();
    REQUIRE(b);
    REQUIRE(b.view() == R"({"req":"hub.set","mode":"periodic"})");
}

TEST_CASE("JsonBuf with runtime values") {
    float temp = 22.5f;
    int humidity = 60;

    note::JsonBuf<256> b;
    b.add("req", "note.add");
    b.add("file", "sensors.qo");
    b.begin_object("body");
        b.add("temperature", static_cast<double>(temp));
        b.add("humidity", humidity);
    b.end_object();
    b.close();

    REQUIRE(b);
    REQUIRE(b.view() ==
        R"({"req":"note.add","file":"sensors.qo","body":{"temperature":22.5,"humidity":60}})");
}

TEST_CASE("JsonBuf overflow reports needed size and capacity") {
    note::JsonBuf<16> b;
    b.add("req", "hub.set");
    b.add("mode", "periodic");
    b.close();

    REQUIRE_FALSE(b);
    REQUIRE(b.overflow());
    CHECK(b.capacity() == 16);
    // size() reports how many bytes would have been written
    CHECK(b.size() > 16);
    // The actual needed size for {"req":"hub.set","mode":"periodic"}
    CHECK(b.size() == 35);
}

TEST_CASE("JsonBuf empty object") {
    note::JsonBuf<8> b;
    b.close();
    REQUIRE(b);
    REQUIRE(b.view() == "{}");
}

TEST_CASE("JsonBuf integer edge cases") {
    note::JsonBuf<64> b;
    b.add("zero", 0);
    b.add("min", INT32_MIN);
    b.add("max", INT32_MAX);
    b.close();
    REQUIRE(b);
    REQUIRE(b.view() == R"({"zero":0,"min":-2147483648,"max":2147483647})");
}

TEST_CASE("JsonBuf double formatting") {
    note::JsonBuf<128> b;
    b.add("a", 0.0);
    b.add("b", 1.0);
    b.add("c", 3.14);
    b.add("d", -0.5);
    b.close();
    REQUIRE(b);
    CHECK(b.view() == R"({"a":0,"b":1,"c":3.14,"d":-0.5})");
}

TEST_CASE("JsonBuf accepts int16_t without cast") {
    int16_t val = 300;
    note::JsonBuf<32> b;
    b.add("x", val);
    b.close();
    REQUIRE(b.view() == R"({"x":300})");
}

TEST_CASE("JsonBuf float value") {
    note::JsonBuf<32> b;
    b.add("x", 1.5f);
    b.close();
    REQUIRE(b.view() == R"({"x":1.5})");
}

TEST_CASE("JsonBuf composed object at runtime") {
    auto body = note::JsonBuf<64>::object();
    body.add("sensor", "bme280");
    body.add("temp", 22.5);
    body.close();

    note::JsonBuf<256> b;
    b.add("req", "note.add");
    b.add("body", body);
    b.close();

    REQUIRE(b);
    REQUIRE(b.view() == R"({"req":"note.add","body":{"sensor":"bme280","temp":22.5}})");
}

TEST_CASE("JsonBuf composed array at runtime") {
    auto files = note::JsonBuf<64>::array();
    files.add("a.qo");
    files.add("b.qo");
    files.close();

    note::JsonBuf<128> b;
    b.add("req", "card.attn");
    b.add("files", files);
    b.close();

    REQUIRE(b);
    REQUIRE(b.view() == R"({"req":"card.attn","files":["a.qo","b.qo"]})");
}

TEST_CASE("JsonBuf standalone array") {
    auto a = note::JsonBuf<32>::array();
    a.add("x");
    a.add("y");
    a.close();
    REQUIRE(a);
    REQUIRE(a.view() == R"(["x","y"])");
}

TEST_CASE("JsonBuf array with bool elements") {
    auto a = note::JsonBuf<32>::array();
    a.add(true);
    a.add(false);
    a.close();
    REQUIRE(a);
    REQUIRE(a.view() == "[true,false]");
}

TEST_CASE("JsonBuf unkeyed int32_t array elements") {
    auto a = note::JsonBuf<32>::array();
    a.add(int32_t{7});
    a.add(int32_t{-3});
    a.close();
    REQUIRE(a.view() == "[7,-3]");
}

TEST_CASE("JsonBuf keyed bool add") {
    note::JsonBuf<32> b;
    b.add("flag", true);
    b.add("done", false);
    b.close();
    REQUIRE(b.view() == R"({"flag":true,"done":false})");
}

TEST_CASE("JsonBuf begin_array and end_array at runtime") {
    note::JsonBuf<64> b;
    b.begin_array("ids");
    b.add("a");
    b.add("b");
    b.end_array();
    b.close();
    REQUIRE(b.view() == R"({"ids":["a","b"]})");
}

TEST_CASE("JsonBuf escape sequences \\r and \\t") {
    note::JsonBuf<64> b;
    b.add("msg", "col1\tcol2\rend");
    b.close();
    REQUIRE(b);
    REQUIRE(b.view() == R"({"msg":"col1\tcol2\rend"})");
}

TEST_CASE("JsonBuf escape sequences quote, backslash, newline") {
    note::JsonBuf<64> b;
    b.add("a", "say \"hi\"");
    b.add("b", "c:\\path");
    b.add("c", "line1\nline2");
    b.close();
    REQUIRE(b.view() == R"({"a":"say \"hi\"","b":"c:\\path","c":"line1\nline2"})");
}

// ── Comprehensive method coverage ───────────────────────────────────────────
// Each distinct JsonBuf<N> generates its own function instantiations. This
// test exercises every public method on a single canonical size (256) so all
// branches are covered without spreading across multiple instantiations.

TEST_CASE("JsonBuf covers all methods on canonical size") {
    using B = note::JsonBuf<256>;

    // Fragment used to exercise the templated add(key, fragment) and add(fragment) overloads.
    auto frag = note::JsonBuf<32>::object();
    frag.add("x", int32_t{1});
    frag.close();

    // ── Object mode (default constructor) ────────────────────────────────
    B b;
    b.add("sv",  std::string_view("hello"));   // add(key, string_view)
    b.add("cs",  "world");                     // add(key, const char*)
    b.add("i32", int32_t{42});                 // add(key, int32_t)
    b.add("i16", int16_t{10});                 // add(key, T) template
    b.add("dbl", 1.5);                         // add(key, double)
    b.add("flt", 2.5f);                        // add(key, float)
    b.add("bl",  true);                        // add(key, bool)
    b.add("frg", frag);                        // add(key, JsonBuf<M>&)
    b.begin_object("obj");
        b.add("inner", "val");
    b.end_object();
    b.begin_array("arr");
        b.add(std::string_view("sv_elem"));    // add(string_view) — unkeyed
        b.add("cs_elem");                      // add(const char*) — unkeyed
        b.add(int32_t{7});                     // add(int32_t) — unkeyed
        b.add(false);                          // add(bool) — unkeyed
        b.add(frag);                           // add(JsonBuf<M>&) — unkeyed
    b.end_array();
    b.close();

    REQUIRE(b);
    REQUIRE_FALSE(b.overflow());
    REQUIRE(b.data() != nullptr);
    REQUIRE(b.size() > 0);
    REQUIRE(b.capacity() == 256);
    REQUIRE(!b.view().empty());

    // ── object() factory ─────────────────────────────────────────────────
    auto obj = B::object();
    obj.add("k", "v");
    obj.close();
    REQUIRE(obj.view() == R"({"k":"v"})");

    // ── array() factory ──────────────────────────────────────────────────
    auto arr = B::array();
    arr.add(std::string_view("a"));
    arr.add("b");
    arr.add(int32_t{1});
    arr.add(true);
    arr.add(frag);
    arr.close();
    REQUIRE(arr);
    REQUIRE(!arr.view().empty());
}
