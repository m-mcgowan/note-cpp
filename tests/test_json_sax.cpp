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
