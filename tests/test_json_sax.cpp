#include "catch.hpp"
#include <note/json_sax.hpp>

#include <string>
#include <vector>

namespace {

struct Event {
    std::string type;
    std::string key;
    std::string value;
};

struct RecordingSink : note::JsonSink {
    std::vector<Event> events;

    void on_null(note::string_view key) override {
        events.push_back({"null", std::string(key), ""});
    }
    void on_bool(note::string_view key, bool value) override {
        events.push_back({"bool", std::string(key), value ? "true" : "false"});
    }
    void on_number(note::string_view key, note::string_view raw) override {
        events.push_back({"number", std::string(key), std::string(raw)});
    }
    void on_string(note::string_view key, note::string_view value) override {
        events.push_back({"string", std::string(key), std::string(value)});
    }
    void on_object_begin(note::string_view key) override {
        events.push_back({"object_begin", std::string(key), ""});
    }
    void on_object_end(note::string_view key) override {
        events.push_back({"object_end", std::string(key), ""});
    }
    void on_array_begin(note::string_view key) override {
        events.push_back({"array_begin", std::string(key), ""});
    }
    void on_array_end(note::string_view key) override {
        events.push_back({"array_end", std::string(key), ""});
    }
};

} // namespace

TEST_CASE("sax_parse: empty object") {
    RecordingSink sink;
    auto err = note::sax_parse("{}", sink);
    REQUIRE(err.empty());
    REQUIRE(sink.events.size() == 2);
    REQUIRE(sink.events[0].type == "object_begin");
    REQUIRE(sink.events[1].type == "object_end");
}

TEST_CASE("sax_parse: string values") {
    RecordingSink sink;
    auto err = note::sax_parse(R"({"name":"Alice","city":"NYC"})", sink);
    REQUIRE(err.empty());
    REQUIRE(sink.events.size() == 4);
    REQUIRE(sink.events[1].type == "string");
    REQUIRE(sink.events[1].key == "name");
    REQUIRE(sink.events[1].value == "Alice");
    REQUIRE(sink.events[2].key == "city");
    REQUIRE(sink.events[2].value == "NYC");
}

TEST_CASE("sax_parse: number values") {
    RecordingSink sink;
    auto err = note::sax_parse(R"({"int":42,"neg":-7,"float":3.14,"exp":1e3})", sink);
    REQUIRE(err.empty());
    REQUIRE(sink.events[1].type == "number");
    REQUIRE(sink.events[1].key == "int");
    REQUIRE(sink.events[1].value == "42");
    REQUIRE(sink.events[2].value == "-7");
    REQUIRE(sink.events[3].value == "3.14");
    REQUIRE(sink.events[4].value == "1e3");
}

TEST_CASE("sax_parse: booleans and null") {
    RecordingSink sink;
    auto err = note::sax_parse(R"({"a":true,"b":false,"c":null})", sink);
    REQUIRE(err.empty());
    REQUIRE(sink.events[1].type == "bool");
    REQUIRE(sink.events[1].value == "true");
    REQUIRE(sink.events[2].type == "bool");
    REQUIRE(sink.events[2].value == "false");
    REQUIRE(sink.events[3].type == "null");
}

TEST_CASE("sax_parse: nested object") {
    RecordingSink sink;
    auto err = note::sax_parse(R"({"body":{"lat":42.5,"lon":-71.1}})", sink);
    REQUIRE(err.empty());
    // root_begin, body_begin, lat, lon, body_end, root_end
    REQUIRE(sink.events[1].type == "object_begin");
    REQUIRE(sink.events[1].key == "body");
    REQUIRE(sink.events[2].type == "number");
    REQUIRE(sink.events[2].key == "lat");
    REQUIRE(sink.events[4].type == "object_end");
    REQUIRE(sink.events[4].key == "body");
}

TEST_CASE("sax_parse: array") {
    RecordingSink sink;
    auto err = note::sax_parse(R"({"items":[1,"two",true]})", sink);
    REQUIRE(err.empty());
    // root_begin, array_begin, 1, "two", true, array_end, root_end
    REQUIRE(sink.events[1].type == "array_begin");
    REQUIRE(sink.events[1].key == "items");
    REQUIRE(sink.events[2].type == "number");
    REQUIRE(sink.events[3].type == "string");
    REQUIRE(sink.events[3].value == "two");
    REQUIRE(sink.events[4].type == "bool");
    REQUIRE(sink.events[5].type == "array_end");
}

TEST_CASE("sax_parse: empty array") {
    RecordingSink sink;
    auto err = note::sax_parse(R"({"a":[]})", sink);
    REQUIRE(err.empty());
    REQUIRE(sink.events[1].type == "array_begin");
    REQUIRE(sink.events[2].type == "array_end");
}

TEST_CASE("sax_parse: escaped strings") {
    RecordingSink sink;
    auto err = note::sax_parse(R"({"msg":"hello\nworld","q":"say \"hi\""})", sink);
    REQUIRE(err.empty());
    REQUIRE(sink.events[1].value == "hello\nworld");
    REQUIRE(sink.events[2].value == "say \"hi\"");
}

TEST_CASE("sax_parse: unicode escape") {
    RecordingSink sink;
    // \u0041 = 'A', \u00E9 = 'é' (2-byte UTF-8), \u4E16 = '世' (3-byte UTF-8)
    auto err = note::sax_parse(R"({"a":"\u0041","b":"\u00E9","c":"\u4E16"})", sink);
    REQUIRE(err.empty());
    REQUIRE(sink.events[1].value == "A");
    REQUIRE(sink.events[2].value == "\xC3\xA9");
    REQUIRE(sink.events[3].value == "\xE4\xB8\x96");
}

TEST_CASE("sax_parse: surrogate pairs produce placeholder") {
    RecordingSink sink;
    auto err = note::sax_parse(R"({"s":"\uD83D"})", sink);
    REQUIRE(err.empty());
    REQUIRE(sink.events[1].value == "?");
}

TEST_CASE("sax_parse: number with exponent sign") {
    RecordingSink sink;
    auto err = note::sax_parse(R"({"a":1.5E+2,"b":1.5e-2})", sink);
    REQUIRE(err.empty());
    REQUIRE(sink.events[1].value == "1.5E+2");
    REQUIRE(sink.events[2].value == "1.5e-2");
}

TEST_CASE("sax_parse: leading zero number") {
    RecordingSink sink;
    auto err = note::sax_parse(R"({"z":0,"f":0.5})", sink);
    REQUIRE(err.empty());
    REQUIRE(sink.events[1].value == "0");
    REQUIRE(sink.events[2].value == "0.5");
}

TEST_CASE("sax_parse: whitespace tolerance") {
    RecordingSink sink;
    auto err = note::sax_parse("  {  \"a\" :  1  }  ", sink);
    REQUIRE(err.empty());
    REQUIRE(sink.events[1].type == "number");
}

// Error cases
TEST_CASE("sax_parse: empty input") {
    RecordingSink sink;
    auto err = note::sax_parse("", sink);
    REQUIRE(!err.empty());
}

TEST_CASE("sax_parse: not an object") {
    RecordingSink sink;
    auto err = note::sax_parse("[1,2]", sink);
    REQUIRE(!err.empty());
}

TEST_CASE("sax_parse: trailing content") {
    RecordingSink sink;
    auto err = note::sax_parse("{}extra", sink);
    REQUIRE(!err.empty());
}

TEST_CASE("sax_parse: unterminated string") {
    RecordingSink sink;
    auto err = note::sax_parse(R"({"a":"hello)", sink);
    REQUIRE(!err.empty());
}

TEST_CASE("sax_parse: missing colon") {
    RecordingSink sink;
    auto err = note::sax_parse(R"({"a" 1})", sink);
    REQUIRE(!err.empty());
}

TEST_CASE("sax_parse: invalid literal") {
    RecordingSink sink;
    auto err = note::sax_parse(R"({"a":tru})", sink);
    REQUIRE(!err.empty());
}

TEST_CASE("sax_parse: unexpected character") {
    RecordingSink sink;
    auto err = note::sax_parse(R"({"a":@})", sink);
    REQUIRE(!err.empty());
}

TEST_CASE("sax_parse: invalid number") {
    RecordingSink sink;
    auto err = note::sax_parse(R"({"a":-})", sink);
    REQUIRE(!err.empty());
}

TEST_CASE("sax_parse: invalid number after dot") {
    RecordingSink sink;
    auto err = note::sax_parse(R"({"a":1.})", sink);
    REQUIRE(!err.empty());
}

TEST_CASE("sax_parse: invalid exponent") {
    RecordingSink sink;
    auto err = note::sax_parse(R"({"a":1e})", sink);
    REQUIRE(!err.empty());
}

TEST_CASE("sax_parse: unterminated escape") {
    RecordingSink sink;
    std::string json = R"({"a":"test\)";
    auto err = note::sax_parse(json, sink);
    REQUIRE(!err.empty());
}

TEST_CASE("sax_parse: invalid hex in unicode escape") {
    RecordingSink sink;
    auto err = note::sax_parse(R"({"a":"\uGGGG"})", sink);
    REQUIRE(!err.empty());
}

TEST_CASE("sax_parse: control character in string") {
    RecordingSink sink;
    std::string json = "{\"a\":\"x\x01y\"}";
    auto err = note::sax_parse(json, sink);
    REQUIRE(!err.empty());
}

TEST_CASE("sax_parse: missing comma between members") {
    RecordingSink sink;
    auto err = note::sax_parse(R"({"a":1 "b":2})", sink);
    REQUIRE(!err.empty());
}

TEST_CASE("sax_parse: missing comma in array") {
    RecordingSink sink;
    auto err = note::sax_parse(R"({"a":[1 2]})", sink);
    REQUIRE(!err.empty());
}

// parse_int / parse_double helpers
TEST_CASE("parse_int: positive") {
    REQUIRE(note::parse_int("42") == 42);
}

TEST_CASE("parse_int: negative") {
    REQUIRE(note::parse_int("-7") == -7);
}

TEST_CASE("parse_int: truncates decimal") {
    REQUIRE(note::parse_int("3.14") == 3);
}

TEST_CASE("parse_int: empty returns default") {
    REQUIRE(note::parse_int("") == 0);
    REQUIRE(note::parse_int("", 99) == 99);
}

TEST_CASE("parse_int: non-numeric returns default") {
    REQUIRE(note::parse_int("abc") == 0);
}

TEST_CASE("parse_double: integer") {
    REQUIRE(note::parse_double("42") == 42.0);
}

TEST_CASE("parse_double: fractional") {
    REQUIRE(note::parse_double("3.14") == Approx(3.14));
}

TEST_CASE("parse_double: negative") {
    REQUIRE(note::parse_double("-2.5") == Approx(-2.5));
}

TEST_CASE("parse_double: exponent") {
    REQUIRE(note::parse_double("1.5e2") == Approx(150.0));
}

TEST_CASE("parse_double: negative exponent") {
    REQUIRE(note::parse_double("1.5e-1") == Approx(0.15));
}

TEST_CASE("parse_double: empty returns default") {
    REQUIRE(note::parse_double("") == 0.0);
    REQUIRE(note::parse_double("", 99.0) == 99.0);
}

// ErrorCaptureSink
TEST_CASE("ErrorCaptureSink: captures err field") {
    RecordingSink inner;
    note::ErrorCaptureSink sink(inner);
    note::sax_parse(R"({"err":"some error","val":42})", sink);
    REQUIRE(sink.captured_error() == "some error");
    // "val" should be forwarded to inner
    REQUIRE(inner.events.size() == 3); // object_begin, number(val), object_end
}

TEST_CASE("ErrorCaptureSink: forwards all non-err events") {
    RecordingSink inner;
    note::ErrorCaptureSink sink(inner);
    note::sax_parse(R"({"a":true,"b":null,"c":[1]})", sink);
    REQUIRE(sink.captured_error().empty());
    // object_begin, bool, null, array_begin, number, array_end, object_end
    REQUIRE(inner.events.size() == 7);
}

// sax_parse overload with const char* + len
TEST_CASE("sax_parse: const char* overload") {
    RecordingSink sink;
    const char* json = R"({"x":1})";
    auto err = note::sax_parse(json, 7, sink);
    REQUIRE(err.empty());
    REQUIRE(sink.events[1].key == "x");
}

// Other escape sequences
TEST_CASE("sax_parse: all escape sequences") {
    RecordingSink sink;
    auto err = note::sax_parse(R"({"s":"a\/b\bc\fd\re\tf"})", sink);
    REQUIRE(err.empty());
    REQUIRE(sink.events[1].value == "a/b\bc\fd\re\tf");
}

// ---------------------------------------------------------------------------
// Branch coverage: escape sequences
// ---------------------------------------------------------------------------

TEST_CASE("sax_parse: unknown escape character falls through to default") {
    // An unrecognized escape like \x should be kept as 'x' (default case in unescape)
    RecordingSink sink;
    auto err = note::sax_parse(R"({"s":"hello\xworld"})", sink);
    REQUIRE(err.empty());
    // The \x in the unescape default case outputs 'x'
    REQUIRE(sink.events[1].value == "helloxworld");
}

TEST_CASE("sax_parse: backslash-backslash escape") {
    RecordingSink sink;
    auto err = note::sax_parse(R"({"s":"a\\b"})", sink);
    REQUIRE(err.empty());
    REQUIRE(sink.events[1].value == "a\\b");
}

// ---------------------------------------------------------------------------
// Branch coverage: number parsing edge cases
// ---------------------------------------------------------------------------

TEST_CASE("sax_parse: negative number") {
    RecordingSink sink;
    auto err = note::sax_parse(R"({"a":-42})", sink);
    REQUIRE(err.empty());
    REQUIRE(sink.events[1].type == "number");
    REQUIRE(sink.events[1].value == "-42");
}

TEST_CASE("sax_parse: exponent with plus sign") {
    RecordingSink sink;
    auto err = note::sax_parse(R"({"a":1e+2})", sink);
    REQUIRE(err.empty());
    REQUIRE(sink.events[1].value == "1e+2");
}

TEST_CASE("sax_parse: uppercase exponent with minus sign") {
    RecordingSink sink;
    auto err = note::sax_parse(R"({"a":1E-3})", sink);
    REQUIRE(err.empty());
    REQUIRE(sink.events[1].value == "1E-3");
}

TEST_CASE("sax_parse: number at exact end of input (no trailing })") {
    // Tests the branch where number parsing reaches end of input
    RecordingSink sink;
    auto err = note::sax_parse(R"({"a":123)", sink);
    // Missing closing brace — the number parses, but object is unterminated
    REQUIRE(!err.empty());
}

TEST_CASE("sax_parse: exponent sign only no digits") {
    RecordingSink sink;
    auto err = note::sax_parse(R"({"a":1e+})", sink);
    REQUIRE(!err.empty());
}

// ---------------------------------------------------------------------------
// Branch coverage: UTF-8 encoding from unicode escapes
// ---------------------------------------------------------------------------

TEST_CASE("sax_parse: unicode single byte (ASCII)") {
    RecordingSink sink;
    auto err = note::sax_parse(R"({"a":"\u0041"})", sink);
    REQUIRE(err.empty());
    REQUIRE(sink.events[1].value == "A");
}

TEST_CASE("sax_parse: unicode 2-byte UTF-8") {
    RecordingSink sink;
    auto err = note::sax_parse(R"({"a":"\u00E9"})", sink);
    REQUIRE(err.empty());
    // é = 0xC3 0xA9
    REQUIRE(sink.events[1].value == "\xC3\xA9");
}

TEST_CASE("sax_parse: unicode 3-byte UTF-8") {
    RecordingSink sink;
    auto err = note::sax_parse(R"({"a":"\u4E16"})", sink);
    REQUIRE(err.empty());
    // 世 = 0xE4 0xB8 0x96
    REQUIRE(sink.events[1].value == "\xE4\xB8\x96");
}

TEST_CASE("sax_parse: unicode surrogate replaced with placeholder") {
    RecordingSink sink;
    auto err = note::sax_parse(R"({"a":"\uD800"})", sink);
    REQUIRE(err.empty());
    REQUIRE(sink.events[1].value == "?");
}

TEST_CASE("sax_parse: unicode high surrogate DFFF") {
    RecordingSink sink;
    auto err = note::sax_parse(R"({"a":"\uDFFF"})", sink);
    REQUIRE(err.empty());
    REQUIRE(sink.events[1].value == "?");
}

TEST_CASE("sax_parse: incomplete unicode escape at end") {
    // \u00 — only 2 hex digits before end of string
    RecordingSink sink;
    std::string json = R"({"a":"\u00"})";
    auto err = note::sax_parse(json, sink);
    // The \u00 has invalid hex (the closing " is not a hex digit)
    REQUIRE(!err.empty());
}

TEST_CASE("sax_parse: unicode escape lowercase hex") {
    RecordingSink sink;
    auto err = note::sax_parse(R"({"a":"\u006f"})", sink);
    REQUIRE(err.empty());
    REQUIRE(sink.events[1].value == "o");
}

// ---------------------------------------------------------------------------
// Branch coverage: truncated literals at end of input
// ---------------------------------------------------------------------------

TEST_CASE("sax_parse: truncated true at end") {
    RecordingSink sink;
    auto err = note::sax_parse(R"({"a":tru)", sink);
    REQUIRE(!err.empty());
}

TEST_CASE("sax_parse: truncated false at end") {
    RecordingSink sink;
    auto err = note::sax_parse(R"({"a":fal)", sink);
    REQUIRE(!err.empty());
}

TEST_CASE("sax_parse: truncated null at end") {
    RecordingSink sink;
    auto err = note::sax_parse(R"({"a":nul)", sink);
    REQUIRE(!err.empty());
}

// ---------------------------------------------------------------------------
// Branch coverage: format_int via on_int (JsonSink default impl)
// ---------------------------------------------------------------------------

TEST_CASE("format_int: zero") {
    char buf[12];
    auto n = note::detail::format_int(buf, 0);
    REQUIRE(n == 1);
    REQUIRE(buf[0] == '0');
}

TEST_CASE("format_int: positive") {
    char buf[12];
    auto n = note::detail::format_int(buf, 42);
    REQUIRE(std::string(buf, n) == "42");
}

TEST_CASE("format_int: negative") {
    char buf[12];
    auto n = note::detail::format_int(buf, -7);
    REQUIRE(std::string(buf, n) == "-7");
}

TEST_CASE("format_int: INT32_MIN") {
    char buf[12];
    auto n = note::detail::format_int(buf, -2147483647 - 1);
    REQUIRE(std::string(buf, n) == "-2147483648");
}

TEST_CASE("format_float: basic") {
    char buf[24];
    auto n = note::detail::format_float(buf, 3.14);
    REQUIRE(n > 0);
    // Just verify it round-trips
    REQUIRE(std::string(buf, n).find("3.14") != std::string::npos);
}

// Test on_int and on_float default implementations via JsonSink
TEST_CASE("JsonSink on_int default forwards to on_number") {
    RecordingSink sink;
    // Call on_int directly — the default impl formats and calls on_number
    sink.on_int("key", 42);
    REQUIRE(sink.events.size() == 1);
    REQUIRE(sink.events[0].type == "number");
    REQUIRE(sink.events[0].key == "key");
    REQUIRE(sink.events[0].value == "42");
}

TEST_CASE("JsonSink on_int with negative value") {
    RecordingSink sink;
    sink.on_int("neg", -99);
    REQUIRE(sink.events.size() == 1);
    REQUIRE(sink.events[0].value == "-99");
}

TEST_CASE("JsonSink on_float default forwards to on_number") {
    RecordingSink sink;
    sink.on_float("pi", 3.14);
    REQUIRE(sink.events.size() == 1);
    REQUIRE(sink.events[0].type == "number");
    REQUIRE(sink.events[0].key == "pi");
    // Just check it produced a number string
    REQUIRE(!sink.events[0].value.empty());
}

// ---------------------------------------------------------------------------
// Branch coverage: parse_double edge cases
// ---------------------------------------------------------------------------

TEST_CASE("parse_double: uppercase E exponent") {
    REQUIRE(note::parse_double("1.5E2") == Approx(150.0));
}

TEST_CASE("parse_double: positive exponent with plus sign") {
    REQUIRE(note::parse_double("1e+2") == Approx(100.0));
}

TEST_CASE("parse_double: integer only (no decimal, no exponent)") {
    REQUIRE(note::parse_double("999") == Approx(999.0));
}

TEST_CASE("parse_double: exponent without decimal part") {
    REQUIRE(note::parse_double("5e3") == Approx(5000.0));
}

// ---------------------------------------------------------------------------
// Branch coverage: parse_int edge cases
// ---------------------------------------------------------------------------

TEST_CASE("parse_int: large negative") {
    REQUIRE(note::parse_int("-1000") == -1000);
}

TEST_CASE("parse_int: zero") {
    REQUIRE(note::parse_int("0") == 0);
}

TEST_CASE("parse_int: negative with decimal truncation") {
    REQUIRE(note::parse_int("-3.9") == -3);
}

// ---------------------------------------------------------------------------
// Branch coverage: ErrorCaptureSinkT (template, non-virtual version)
// ---------------------------------------------------------------------------

TEST_CASE("ErrorCaptureSinkT: captures err from template sink") {
    note::NullSink inner;
    note::ErrorCaptureSinkT<note::NullSink> sink(inner);
    // Manually deliver events as the SAX parser would
    sink.on_string("err", "device error");
    REQUIRE(sink.captured_error() == "device error");
}

TEST_CASE("ErrorCaptureSinkT: forwards non-err strings") {
    // Use a simple recording struct
    struct CountingSink : note::NullSink {
        int string_count = 0;
        void on_string(note::string_view, note::string_view) { ++string_count; }
    };
    CountingSink inner;
    note::ErrorCaptureSinkT<CountingSink> sink(inner);
    sink.on_string("name", "Alice");
    REQUIRE(sink.captured_error().empty());
    REQUIRE(inner.string_count == 1);
}

TEST_CASE("ErrorCaptureSinkT: reset clears error") {
    note::NullSink inner;
    note::ErrorCaptureSinkT<note::NullSink> sink(inner);
    sink.on_string("err", "some error");
    REQUIRE(!sink.captured_error().empty());
    sink.reset();
    REQUIRE(sink.captured_error().empty());
}

TEST_CASE("ErrorCaptureSinkT: truncates long error") {
    note::NullSink inner;
    note::ErrorCaptureSinkT<note::NullSink> sink(inner);
    // Error longer than kMaxErrLen (64)
    std::string long_err(100, 'x');
    sink.on_string("err", note::string_view(long_err.data(), long_err.size()));
    REQUIRE(sink.captured_error().size() == 64);
}

// ---------------------------------------------------------------------------
// Branch coverage: ErrorCaptureSink (virtual version) reset
// ---------------------------------------------------------------------------

TEST_CASE("ErrorCaptureSink: reset clears captured error") {
    RecordingSink inner;
    note::ErrorCaptureSink sink(inner);
    note::sax_parse(R"({"err":"oops"})", sink);
    REQUIRE(sink.captured_error() == "oops");
    sink.reset();
    REQUIRE(sink.captured_error().empty());
}

// ---------------------------------------------------------------------------
// Branch coverage: FilterSink template
// ---------------------------------------------------------------------------

TEST_CASE("FilterSink: forwards all event types") {
    struct TrackingSink : note::NullSink {
        int null_count = 0;
        int bool_count = 0;
        int number_count = 0;
        int int_count = 0;
        int float_count = 0;
        int string_count = 0;
        int obj_begin_count = 0;
        int obj_end_count = 0;
        int arr_begin_count = 0;
        int arr_end_count = 0;
        bool was_reset = false;

        void on_null(note::string_view) { ++null_count; }
        void on_bool(note::string_view, bool) { ++bool_count; }
        void on_number(note::string_view, note::string_view) { ++number_count; }
        void on_int(note::string_view, note::json_int_t) { ++int_count; }
        void on_float(note::string_view, double) { ++float_count; }
        void on_string(note::string_view, note::string_view) { ++string_count; }
        void on_object_begin(note::string_view) { ++obj_begin_count; }
        void on_object_end(note::string_view) { ++obj_end_count; }
        void on_array_begin(note::string_view) { ++arr_begin_count; }
        void on_array_end(note::string_view) { ++arr_end_count; }
        void reset() { was_reset = true; }
    };

    TrackingSink inner;
    note::FilterSink<TrackingSink> filter(inner);

    filter.on_null("k");
    filter.on_bool("k", true);
    filter.on_number("k", "42");
    filter.on_int("k", 7);
    filter.on_float("k", 1.5);
    filter.on_string("k", "v");
    filter.on_object_begin("k");
    filter.on_object_end("k");
    filter.on_array_begin("k");
    filter.on_array_end("k");
    filter.reset();

    REQUIRE(inner.null_count == 1);
    REQUIRE(inner.bool_count == 1);
    REQUIRE(inner.number_count == 1);
    REQUIRE(inner.int_count == 1);
    REQUIRE(inner.float_count == 1);
    REQUIRE(inner.string_count == 1);
    REQUIRE(inner.obj_begin_count == 1);
    REQUIRE(inner.obj_end_count == 1);
    REQUIRE(inner.arr_begin_count == 1);
    REQUIRE(inner.arr_end_count == 1);
    REQUIRE(inner.was_reset);
}

// ---------------------------------------------------------------------------
// Branch coverage: JsonSinkAdapter
// ---------------------------------------------------------------------------

TEST_CASE("JsonSinkAdapter: adapts concrete sink to virtual JsonSink") {
    struct SimpleSink {
        int calls = 0;
        void on_null(note::string_view) { ++calls; }
        void on_bool(note::string_view, bool) { ++calls; }
        void on_number(note::string_view, note::string_view) { ++calls; }
        void on_int(note::string_view, note::json_int_t) { ++calls; }
        void on_float(note::string_view, double) { ++calls; }
        void on_string(note::string_view, note::string_view) { ++calls; }
        void on_object_begin(note::string_view) { ++calls; }
        void on_object_end(note::string_view) { ++calls; }
        void on_array_begin(note::string_view) { ++calls; }
        void on_array_end(note::string_view) { ++calls; }
        void reset() { ++calls; }
    };

    SimpleSink concrete;
    note::JsonSinkAdapter<SimpleSink> adapter(concrete);

    // Use as a JsonSink reference (virtual dispatch)
    note::JsonSink& virt = adapter;
    virt.on_null("k");
    virt.on_bool("k", false);
    virt.on_number("k", "1");
    virt.on_int("k", 5);
    virt.on_float("k", 2.0);
    virt.on_string("k", "v");
    virt.on_object_begin("k");
    virt.on_object_end("k");
    virt.on_array_begin("k");
    virt.on_array_end("k");
    virt.reset();

    REQUIRE(concrete.calls == 11);
}

// ---------------------------------------------------------------------------
// Branch coverage: scratch buffer overflow in unescape
// ---------------------------------------------------------------------------

TEST_CASE("sax_parse: 2-byte unicode at scratch boundary still fits") {
    // Scratch buffer is 256 bytes. Loop condition: out < 255.
    // With 254 plain chars: out=254, enters loop, \u00E9 check: out+1=255 < 256 → true.
    // The 2-byte char fits, producing 256 bytes total.
    std::string value(254, 'A');
    value += "\\u00E9";  // 2-byte UTF-8, fits at position 254
    std::string json = R"({"s":")" + value + R"("})";
    RecordingSink sink;
    auto err = note::sax_parse(json, sink);
    REQUIRE(err.empty());
    // 254 A's + 2 bytes for é = 256
    REQUIRE(sink.events[1].value.size() == 256);
}

TEST_CASE("sax_parse: long string with 3-byte unicode near scratch limit") {
    // Similar to above but for 3-byte UTF-8: need out >= 254 so out+2 >= 256
    // Use 254 chars to get out=254, then \u4E16 → out+2=256 !< 256 → skip
    std::string value(254, 'B');
    value += "\\u4E16";  // 3-byte UTF-8 — out+2=256 !< 256 → skip
    std::string json = R"({"s":")" + value + R"("})";
    RecordingSink sink;
    auto err = note::sax_parse(json, sink);
    REQUIRE(err.empty());
    // 254 B's; the 3-byte unicode char is skipped because out+2 >= kScratchSize
    REQUIRE(sink.events[1].value.size() == 254);
}

// ---------------------------------------------------------------------------
// Branch coverage: multiple escapes in one string
// ---------------------------------------------------------------------------

TEST_CASE("sax_parse: string with mixed escapes and unicode") {
    RecordingSink sink;
    auto err = note::sax_parse(R"({"s":"a\nb\tc\/d\u0041"})", sink);
    REQUIRE(err.empty());
    REQUIRE(sink.events[1].value == "a\nb\tc/dA");
}

// ---------------------------------------------------------------------------
// Branch coverage: FilterJsonSink (virtual version) all methods
// ---------------------------------------------------------------------------

TEST_CASE("FilterJsonSink: forwards all events to inner") {
    RecordingSink inner;
    note::FilterJsonSink filter(inner);

    filter.on_null("k");
    filter.on_bool("k", true);
    filter.on_number("k", "1");
    filter.on_int("k", 5);
    filter.on_float("k", 2.0);
    filter.on_string("k", "v");
    filter.on_object_begin("k");
    filter.on_object_end("k");
    filter.on_array_begin("k");
    filter.on_array_end("k");
    filter.reset();

    // on_int and on_float go through default impl -> on_number
    // So: null, bool, number(raw), number(from int), number(from float),
    //     string, obj_begin, obj_end, arr_begin, arr_end
    REQUIRE(inner.events.size() == 10);
}

// ---------------------------------------------------------------------------
// Branch coverage: deeply nested object keys
// ---------------------------------------------------------------------------

TEST_CASE("sax_parse: nested array with objects") {
    RecordingSink sink;
    auto err = note::sax_parse(R"({"arr":[{"x":1},{"y":2}]})", sink);
    REQUIRE(err.empty());
    // root_begin, arr_begin, obj_begin, x:1, obj_end, obj_begin, y:2, obj_end, arr_end, root_end
    CHECK(sink.events[0].type == "object_begin");
    CHECK(sink.events[1].type == "array_begin");
    CHECK(sink.events[2].type == "object_begin");
    CHECK(sink.events[3].type == "number");
    CHECK(sink.events[3].key == "x");
    CHECK(sink.events[4].type == "object_end");
    CHECK(sink.events[9].type == "object_end");
}

// ---------------------------------------------------------------------------
// Branch coverage: edge case — empty string value
// ---------------------------------------------------------------------------

TEST_CASE("sax_parse: empty string value") {
    RecordingSink sink;
    auto err = note::sax_parse(R"({"a":""})", sink);
    REQUIRE(err.empty());
    REQUIRE(sink.events[1].value.empty());
}

// ---------------------------------------------------------------------------
// Branch coverage: null as only value
// ---------------------------------------------------------------------------

TEST_CASE("sax_parse: null truncated at very end of buffer") {
    RecordingSink sink;
    // "nul" without the final 'l' — should error
    std::string json = R"({"a":nul)";
    auto err = note::sax_parse(json, sink);
    REQUIRE(!err.empty());
}

// ---------------------------------------------------------------------------
// Branch coverage: false literal mismatch
// ---------------------------------------------------------------------------

TEST_CASE("sax_parse: false literal mismatch") {
    RecordingSink sink;
    auto err = note::sax_parse(R"({"a":faxse})", sink);
    REQUIRE(!err.empty());
}

// ---------------------------------------------------------------------------
// Branch coverage: null literal mismatch
// ---------------------------------------------------------------------------

TEST_CASE("sax_parse: null literal mismatch") {
    RecordingSink sink;
    auto err = note::sax_parse(R"({"a":noll})", sink);
    REQUIRE(!err.empty());
}

// ═══════════════════════════════════════════════════════════════════════
// Branch coverage — string escape sequences in unescape()
// ═══════════════════════════════════════════════════════════════════════

TEST_CASE("sax_parse: escape tab and carriage return") {
    RecordingSink sink;
    auto err = note::sax_parse(R"({"v":"a\tb\rc"})", sink);
    REQUIRE(err.empty());
    // events: object_begin, string("v"), object_end
    auto it = std::find_if(sink.events.begin(), sink.events.end(),
        [](const Event& e) { return e.type == "string"; });
    REQUIRE(it != sink.events.end());
    CHECK(it->value == "a\tb\rc");
}

TEST_CASE("sax_parse: escape backspace and formfeed") {
    RecordingSink sink;
    auto err = note::sax_parse(R"({"v":"\b\f"})", sink);
    REQUIRE(err.empty());
    auto it = std::find_if(sink.events.begin(), sink.events.end(),
        [](const Event& e) { return e.type == "string"; });
    REQUIRE(it != sink.events.end());
    CHECK(it->value == "\b\f");
}

TEST_CASE("sax_parse: escape backslash and forward slash") {
    RecordingSink sink;
    auto err = note::sax_parse(R"({"v":"\\\/"})", sink);
    REQUIRE(err.empty());
    auto it = std::find_if(sink.events.begin(), sink.events.end(),
        [](const Event& e) { return e.type == "string"; });
    REQUIRE(it != sink.events.end());
    CHECK(it->value == "\\/");
}

TEST_CASE("sax_parse: unicode escape \\u0041 = A (ASCII)") {
    RecordingSink sink;
    auto err = note::sax_parse(R"({"v":"\u0041"})", sink);
    REQUIRE(err.empty());
    auto it = std::find_if(sink.events.begin(), sink.events.end(),
        [](const Event& e) { return e.type == "string"; });
    REQUIRE(it != sink.events.end());
    CHECK(it->value == "A");
}

TEST_CASE("sax_parse: unicode escape \\u00E9 (2-byte UTF-8)") {
    RecordingSink sink;
    auto err = note::sax_parse(R"({"v":"\u00e9"})", sink);
    REQUIRE(err.empty());
    auto it = std::find_if(sink.events.begin(), sink.events.end(),
        [](const Event& e) { return e.type == "string"; });
    REQUIRE(it != sink.events.end());
    // é = U+00E9 = 0xC3 0xA9 in UTF-8
    CHECK(it->value.size() == 2);
    CHECK(static_cast<unsigned char>(it->value[0]) == 0xC3);
    CHECK(static_cast<unsigned char>(it->value[1]) == 0xA9);
}

TEST_CASE("sax_parse: unicode escape \\u4E16 (3-byte UTF-8)") {
    RecordingSink sink;
    auto err = note::sax_parse(R"({"v":"\u4E16"})", sink);
    REQUIRE(err.empty());
    auto it = std::find_if(sink.events.begin(), sink.events.end(),
        [](const Event& e) { return e.type == "string"; });
    REQUIRE(it != sink.events.end());
    // 世 = U+4E16 = 0xE4 0xB8 0x96 in UTF-8
    CHECK(it->value.size() == 3);
    CHECK(static_cast<unsigned char>(it->value[0]) == 0xE4);
}

TEST_CASE("sax_parse: unicode surrogate replaced with ?") {
    RecordingSink sink;
    auto err = note::sax_parse(R"({"v":"\uD800"})", sink);
    REQUIRE(err.empty());
    auto it = std::find_if(sink.events.begin(), sink.events.end(),
        [](const Event& e) { return e.type == "string"; });
    REQUIRE(it != sink.events.end());
    CHECK(it->value == "?");
}

// ═══════════════════════════════════════════════════════════════════════
// Branch coverage — number parsing edge cases
// ═══════════════════════════════════════════════════════════════════════

TEST_CASE("sax_parse: number with exponent") {
    RecordingSink sink;
    auto err = note::sax_parse(R"({"v":1.5e2})", sink);
    REQUIRE(err.empty());
    auto it = std::find_if(sink.events.begin(), sink.events.end(),
        [](const Event& e) { return e.type == "number"; });
    REQUIRE(it != sink.events.end());
    CHECK(it->value == "1.5e2");
}

TEST_CASE("sax_parse: number with positive exponent") {
    RecordingSink sink;
    auto err = note::sax_parse(R"({"v":1e+3})", sink);
    REQUIRE(err.empty());
    auto it = std::find_if(sink.events.begin(), sink.events.end(),
        [](const Event& e) { return e.type == "number"; });
    REQUIRE(it != sink.events.end());
    CHECK(it->value == "1e+3");
}

TEST_CASE("sax_parse: number with negative exponent") {
    RecordingSink sink;
    auto err = note::sax_parse(R"({"v":1e-3})", sink);
    REQUIRE(err.empty());
}

TEST_CASE("sax_parse: leading zero with fraction") {
    RecordingSink sink;
    auto err = note::sax_parse(R"({"v":0.5})", sink);
    REQUIRE(err.empty());
}

TEST_CASE("sax_parse: tab in whitespace positions") {
    RecordingSink sink;
    auto err = note::sax_parse("{\t\"a\"\t:\t1\t}", sink);
    REQUIRE(err.empty());
}

TEST_CASE("sax_parse: carriage return in whitespace") {
    RecordingSink sink;
    auto err = note::sax_parse("{\r\"a\"\r:\r1\r}", sink);
    REQUIRE(err.empty());
}

TEST_CASE("sax_parse: truncated JSON at various points") {
    // Exercises peek()/advance() returning '\0' at end of input.
    RecordingSink sink;
    // Truncated after opening brace
    CHECK(!note::sax_parse("{", 1, sink).empty());
    // Truncated mid-key
    CHECK(!note::sax_parse("{\"a", 3, sink).empty());
    // Truncated after colon
    CHECK(!note::sax_parse("{\"a\":", 5, sink).empty());
    // Truncated mid-number
    CHECK(!note::sax_parse("{\"a\":1", 6, sink).empty());
    // Truncated mid-string value
    CHECK(!note::sax_parse("{\"a\":\"b", 7, sink).empty());
    // Truncated mid-escape
    CHECK(!note::sax_parse("{\"a\":\"\\", 7, sink).empty());
}
