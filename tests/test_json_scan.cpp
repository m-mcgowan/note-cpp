// Tests for note::scan — lightweight JSON field extraction for
// known response shapes (no SAX parser). Covers the structural
// primitives (field/object/array) and the typed get<T>() extractor.
#include <doctest.h>
#include <note/json_scan.hpp>
#include <note/json_view.hpp>
#include <note/body.hpp>
#include <optional>
#include <string>

using note::string_view;

// ---------------------------------------------------------------------------
// note::scan::field — substring search for "key":value
// ---------------------------------------------------------------------------
TEST_CASE("scan::field finds flat string value", "[json_scan]") {
    auto v = note::scan::field(R"({"name":"abc","x":1})", "name");
    REQUIRE(v == R"("abc")");
}

TEST_CASE("scan::field finds flat number value", "[json_scan]") {
    auto v = note::scan::field(R"({"x":42,"y":"z"})", "x");
    REQUIRE(v == "42");
}

TEST_CASE("scan::field stops at trailing comma", "[json_scan]") {
    auto v = note::scan::field(R"({"a":1,"b":2})", "a");
    REQUIRE(v == "1");
}

TEST_CASE("scan::field stops at closing brace", "[json_scan]") {
    auto v = note::scan::field(R"({"only":7})", "only");
    REQUIRE(v == "7");
}

TEST_CASE("scan::field returns empty for missing key", "[json_scan]") {
    auto v = note::scan::field(R"({"x":1})", "missing");
    REQUIRE(v.empty());
}

TEST_CASE("scan::field skips whitespace after colon", "[json_scan]") {
    auto v = note::scan::field(R"({"x":   123})", "x");
    REQUIRE(v == "123");
}

TEST_CASE("scan::field tolerates whitespace around key", "[json_scan]") {
    auto v = note::scan::field(R"({ "x" : 5 })", "x");
    REQUIRE(v == "5");
}

TEST_CASE("scan::field returns object substring for object value", "[json_scan]") {
    auto v = note::scan::field(R"({"body":{"a":1}})", "body");
    REQUIRE(v == R"({"a":1})");
}

TEST_CASE("scan::field returns array substring for array value", "[json_scan]") {
    auto v = note::scan::field(R"({"xs":[1,2,3]})", "xs");
    REQUIRE(v == "[1,2,3]");
}

// ---------------------------------------------------------------------------
// note::scan::object — extract {...} for a given key (depth-counted)
// ---------------------------------------------------------------------------
TEST_CASE("scan::object returns flat object", "[json_scan]") {
    auto v = note::scan::object(R"({"body":{"temp":21.5}})", "body");
    REQUIRE(v == R"({"temp":21.5})");
}

TEST_CASE("scan::object handles nested braces", "[json_scan]") {
    auto v = note::scan::object(R"({"body":{"inner":{"x":1},"y":2}})", "body");
    REQUIRE(v == R"({"inner":{"x":1},"y":2})");
}

TEST_CASE("scan::object returns empty when key missing", "[json_scan]") {
    auto v = note::scan::object(R"({"body":{"x":1}})", "missing");
    REQUIRE(v.empty());
}

TEST_CASE("scan::object returns empty when value is not an object", "[json_scan]") {
    auto v = note::scan::object(R"({"x":42})", "x");
    REQUIRE(v.empty());
}

// ---------------------------------------------------------------------------
// note::scan::array — extract [...] for a given key
// ---------------------------------------------------------------------------
TEST_CASE("scan::array returns flat array", "[json_scan]") {
    auto v = note::scan::array(R"({"xs":[1,2,3]})", "xs");
    REQUIRE(v == "[1,2,3]");
}

TEST_CASE("scan::array handles nested arrays", "[json_scan]") {
    auto v = note::scan::array(R"({"xs":[[1,2],[3,4]]})", "xs");
    REQUIRE(v == "[[1,2],[3,4]]");
}

TEST_CASE("scan::array returns empty when key missing", "[json_scan]") {
    auto v = note::scan::array(R"({"xs":[1]})", "ys");
    REQUIRE(v.empty());
}

TEST_CASE("scan::array returns empty when value is not an array", "[json_scan]") {
    auto v = note::scan::array(R"({"x":42})", "x");
    REQUIRE(v.empty());
}

// ---------------------------------------------------------------------------
// note::scan::get<T> — typed extraction with default fallback
// ---------------------------------------------------------------------------
TEST_CASE("scan::get<int> extracts integer", "[json_scan]") {
    auto v = note::scan::get(R"({"n":42})", "n", int32_t{0});
    REQUIRE(v == 42);
}

TEST_CASE("scan::get<int> returns default for missing key", "[json_scan]") {
    auto v = note::scan::get(R"({"x":1})", "n", int32_t{7});
    REQUIRE(v == 7);
}

TEST_CASE("scan::get<double> extracts decimal", "[json_scan]") {
    auto v = note::scan::get(R"({"t":21.5})", "t", 0.0);
    REQUIRE(v == doctest::Approx(21.5));
}

TEST_CASE("scan::get<float> extracts float", "[json_scan]") {
    auto v = note::scan::get(R"({"t":3.25})", "t", 0.0f);
    REQUIRE(v == doctest::Approx(3.25f));
}

TEST_CASE("scan::get<bool> extracts true", "[json_scan]") {
    auto v = note::scan::get(R"({"on":true})", "on", false);
    REQUIRE(v == true);
}

TEST_CASE("scan::get<bool> extracts false", "[json_scan]") {
    auto v = note::scan::get(R"({"on":false})", "on", true);
    REQUIRE(v == false);
}

TEST_CASE("scan::get<bool> returns default for missing", "[json_scan]") {
    auto v = note::scan::get(R"({"x":1})", "on", true);
    REQUIRE(v == true);
}

TEST_CASE("scan::get<string_view> returns value without quotes", "[json_scan]") {
    auto v = note::scan::get(R"({"name":"abc"})", "name", string_view{"?"});
    REQUIRE(v == "abc");
}

TEST_CASE("scan::get<string_view> returns default for missing", "[json_scan]") {
    auto v = note::scan::get(R"({"x":1})", "name", string_view{"?"});
    REQUIRE(v == "?");
}

// ---------------------------------------------------------------------------
// Named variants: scan::get_int / get_double / get_float / get_bool / get_str
// ---------------------------------------------------------------------------
TEST_CASE("scan::get_int extracts integer", "[json_scan]") {
    REQUIRE(note::scan::get_int(R"({"n":42})", "n") == 42);
    REQUIRE(note::scan::get_int(R"({"x":1})", "missing", 7) == 7);
}

TEST_CASE("scan::get_double extracts decimal", "[json_scan]") {
    REQUIRE(note::scan::get_double(R"({"t":21.5})", "t") == doctest::Approx(21.5));
}

TEST_CASE("scan::get_float extracts float", "[json_scan]") {
    REQUIRE(note::scan::get_float(R"({"t":3.25})", "t") == doctest::Approx(3.25f));
}

TEST_CASE("scan::get_bool extracts and defaults", "[json_scan]") {
    REQUIRE(note::scan::get_bool(R"({"on":true})",  "on") == true);
    REQUIRE(note::scan::get_bool(R"({"on":false})", "on") == false);
    REQUIRE(note::scan::get_bool(R"({"x":1})",      "missing", true) == true);
}

TEST_CASE("scan::get_str returns unquoted string", "[json_scan]") {
    REQUIRE(note::scan::get_str(R"({"name":"abc"})", "name") == "abc");
    REQUIRE(note::scan::get_str(R"({"x":1})",        "name", "?") == "?");
}

TEST_CASE("JsonView named variants", "[json_view]") {
    note::JsonView v(R"({"n":42,"t":3.25,"on":true,"name":"abc"})");
    REQUIRE(v.get_int   ("n") == 42);
    REQUIRE(v.get_float ("t") == doctest::Approx(3.25f));
    REQUIRE(v.get_bool  ("on") == true);
    REQUIRE(v.get_str   ("name") == "abc");
    REQUIRE(v.get_int   ("missing", 9) == 9);
}

// ---------------------------------------------------------------------------
// Nested traversal via scan::object + scan::get
// ---------------------------------------------------------------------------
TEST_CASE("nested body extraction", "[json_scan]") {
    const char* resp = R"({"body":{"temperature":21.5,"humidity":60},"other":"x"})";
    auto body = note::scan::object(resp, "body");
    REQUIRE(!body.empty());
    float t = note::scan::get(body, "temperature", 0.0f);
    int32_t h = note::scan::get(body, "humidity", int32_t{0});
    REQUIRE(t == doctest::Approx(21.5f));
    REQUIRE(h == 60);
}

// ---------------------------------------------------------------------------
// note::JsonView — thin class wrapper around note::scan
// ---------------------------------------------------------------------------
TEST_CASE("JsonView extracts typed field", "[json_view]") {
    note::JsonView v(R"({"temp":21.5,"on":true})");
    REQUIRE(v.get("temp", 0.0) == doctest::Approx(21.5));
    REQUIRE(v.get("on", false) == true);
}

TEST_CASE("JsonView chains object traversal", "[json_view]") {
    note::JsonView root(R"({"body":{"temp":4.2,"name":"sensor-1"}})");
    auto body = root.object("body");
    REQUIRE(body.get("temp", 0.0) == doctest::Approx(4.2));
    REQUIRE(body.get<string_view>("name", "?") == "sensor-1");
}

TEST_CASE("JsonView returns empty view for missing object", "[json_view]") {
    note::JsonView root(R"({"x":1})");
    auto missing = root.object("body");
    REQUIRE(missing.empty());
    REQUIRE(missing.get("temp", 9.9) == doctest::Approx(9.9));
}

TEST_CASE("JsonView array accessor", "[json_view]") {
    note::JsonView root(R"({"xs":[1,2,3]})");
    auto xs = root.array("xs");
    REQUIRE(xs == "[1,2,3]");
}

// ---------------------------------------------------------------------------
// note::scan::for_each — single-pass visitor over top-level pairs
// ---------------------------------------------------------------------------
TEST_CASE("scan::for_each visits all top-level pairs", "[json_scan]") {
    std::string keys;
    std::string values;
    note::scan::for_each(R"({"a":1,"b":"two","c":true})",
        [&](note::string_view k, note::string_view v) {
            keys.append(k).append("|");
            values.append(v).append("|");
        });
    REQUIRE(keys == "a|b|c|");
    REQUIRE(values == R"(1|"two"|true|)");
}

TEST_CASE("scan::for_each skips nested objects as single values", "[json_scan]") {
    std::string body, tail;
    int n = 0;
    note::scan::for_each(R"({"body":{"inner":1,"deep":{"x":2}},"tail":9})",
        [&](note::string_view k, note::string_view v) {
            ++n;
            if (k == "body") body = v;
            else if (k == "tail") tail = v;
        });
    REQUIRE(n == 2);
    REQUIRE(body == R"({"inner":1,"deep":{"x":2}})");
    REQUIRE(tail == "9");
}

TEST_CASE("scan::for_each handles whitespace", "[json_scan]") {
    int n = 0;
    note::scan::for_each("{ \"a\" : 1 , \"b\" : 2 }",
        [&](note::string_view, note::string_view) { ++n; });
    REQUIRE(n == 2);
}

TEST_CASE("scan::for_each on empty object", "[json_scan]") {
    int n = 0;
    note::scan::for_each("{}",
        [&](note::string_view, note::string_view) { ++n; });
    REQUIRE(n == 0);
}

TEST_CASE("scan::for_each stops at matching brace", "[json_scan]") {
    // Should NOT visit pairs in the outer object after the inner closes.
    int n = 0;
    note::scan::for_each(R"({"a":{"x":1},"b":2})",
        [&](note::string_view, note::string_view) { ++n; });
    REQUIRE(n == 2);
}

// ---------------------------------------------------------------------------
// note::scan::into — populate a NOTE_FIELDS struct in one pass
// ---------------------------------------------------------------------------
namespace {
struct Readings {
    float temp;
    int32_t humidity;
    bool alarm;
    NOTE_FIELDS(temp, humidity, alarm)
};
} // namespace

TEST_CASE("scan::into populates all fields", "[json_scan]") {
    Readings r{0.0f, 0, false};
    note::scan::into(R"({"temp":21.5,"humidity":60,"alarm":true})", r);
    REQUIRE(r.temp == doctest::Approx(21.5f));
    REQUIRE(r.humidity == 60);
    REQUIRE(r.alarm == true);
}

TEST_CASE("scan::into ignores unknown keys", "[json_scan]") {
    Readings r{0.0f, 0, false};
    note::scan::into(R"({"temp":4.2,"extra":"x","humidity":10})", r);
    REQUIRE(r.temp == doctest::Approx(4.2f));
    REQUIRE(r.humidity == 10);
    REQUIRE(r.alarm == false);
}

TEST_CASE("scan::into leaves missing fields untouched", "[json_scan]") {
    Readings r{9.9f, 99, true};
    note::scan::into(R"({"temp":1.0})", r);
    REQUIRE(r.temp == doctest::Approx(1.0f));
    REQUIRE(r.humidity == 99);   // untouched
    REQUIRE(r.alarm == true);    // untouched
}

TEST_CASE("JsonView::into forwards to scan::into", "[json_view]") {
    note::JsonView v(R"({"temp":3.14,"humidity":45,"alarm":false})");
    Readings r{};
    v.into(r);
    REQUIRE(r.temp == doctest::Approx(3.14f));
    REQUIRE(r.humidity == 45);
    REQUIRE(r.alarm == false);
}

TEST_CASE("scan::into with pick tag matches walk result", "[json_scan]") {
    const char* j = R"({"temp":2.75,"humidity":33,"alarm":true})";
    Readings walk_r{}, pick_r{};
    note::scan::into(j, walk_r);                      // default (walk)
    note::scan::into(j, pick_r, note::scan::pick);    // explicit (pick)
    REQUIRE(walk_r.temp     == doctest::Approx(pick_r.temp));
    REQUIRE(walk_r.humidity == pick_r.humidity);
    REQUIRE(walk_r.alarm    == pick_r.alarm);
    REQUIRE(pick_r.temp     == doctest::Approx(2.75f));
    REQUIRE(pick_r.humidity == 33);
    REQUIRE(pick_r.alarm    == true);
}

TEST_CASE("JsonView::into with pick tag", "[json_view]") {
    note::JsonView v(R"({"temp":1.5,"humidity":20,"alarm":false})");
    Readings r{};
    v.into(r, note::scan::pick);
    REQUIRE(r.temp == doctest::Approx(1.5f));
    REQUIRE(r.humidity == 20);
    REQUIRE(r.alarm == false);
}

TEST_CASE("scan tag via using alias", "[json_scan]") {
    // Users can swap strategies by redeclaring a type alias:
    using scan_type = note::scan::pick_t;
    Readings r{};
    note::scan::into(R"({"temp":4.0,"humidity":50,"alarm":true})", r, scan_type{});
    REQUIRE(r.temp == doctest::Approx(4.0f));
    REQUIRE(r.humidity == 50);
    REQUIRE(r.alarm == true);
}

TEST_CASE("JsonView constructs from std::optional<string_view>", "[json_view]") {
    std::optional<note::string_view> ok{note::string_view(R"({"x":42})")};
    std::optional<note::string_view> empty{};
    REQUIRE(note::JsonView(ok).get_int("x") == 42);
    REQUIRE(note::JsonView(empty).get_int("x", -1) == -1);
    REQUIRE(note::JsonView(empty).empty());
}

// ---------------------------------------------------------------------------
// Flash-string key overloads — keys stored in flash (PROGMEM on AVR),
// verified here by passing a note::FlashString directly. On host builds
// this exercises the same code path that Arduino F("...") hits via the
// __FlashStringHelper* overload.
// ---------------------------------------------------------------------------
static constexpr char k_temp[]     = "temperature";
static constexpr char k_humidity[] = "humidity";
static constexpr char k_body[]     = "body";
static constexpr char k_missing[]  = "missing";

TEST_CASE("scan::field accepts FlashString key", "[json_scan][flash]") {
    auto v = note::scan::field(R"({"temperature":21.5,"humidity":60})",
                               note::flash(k_temp));
    REQUIRE(v == "21.5");
}

TEST_CASE("scan::object accepts FlashString key", "[json_scan][flash]") {
    auto v = note::scan::object(R"({"body":{"a":1}})", note::flash(k_body));
    REQUIRE(v == R"({"a":1})");
}

TEST_CASE("scan::get_float accepts FlashString key", "[json_scan][flash]") {
    auto v = note::scan::get_float(R"({"temperature":4.2})",
                                   note::flash(k_temp));
    REQUIRE(v == doctest::Approx(4.2f));
}

TEST_CASE("scan::get_int accepts FlashString key + default", "[json_scan][flash]") {
    auto hit  = note::scan::get_int(R"({"humidity":60})",  note::flash(k_humidity));
    auto miss = note::scan::get_int(R"({"humidity":60})", note::flash(k_missing), 99);
    REQUIRE(hit  == 60);
    REQUIRE(miss == 99);
}

TEST_CASE("JsonView chains with flash keys", "[json_view][flash]") {
    note::JsonView root(R"({"body":{"temperature":21.5,"humidity":60}})");
    auto body = root.object(note::flash(k_body));
    REQUIRE(body.get_float(note::flash(k_temp))    == doctest::Approx(21.5f));
    REQUIRE(body.get_int  (note::flash(k_humidity)) == 60);
}

TEST_CASE("JsonView::for_each visits pairs", "[json_view]") {
    note::JsonView v(R"({"a":1,"b":2})");
    int n = 0;
    v.for_each([&](note::string_view, note::string_view) { ++n; });
    REQUIRE(n == 2);
}
