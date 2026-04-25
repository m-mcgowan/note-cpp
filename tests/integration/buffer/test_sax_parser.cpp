// Integration tests for the SAX JSON parser.

#include <doctest.h>

#include <note/json_sax.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace note;

// ---------------------------------------------------------------------------
// Test sink that records all events for verification.
// ---------------------------------------------------------------------------
struct RecordingSink : JsonSink {
    struct Event {
        std::string type;
        std::string key;
        std::string value;
    };
    std::vector<Event> events;

    void on_null(string_view key) override {
        events.push_back({"null", std::string(key), ""});
    }
    void on_bool(string_view key, bool value) override {
        events.push_back({"bool", std::string(key), value ? "true" : "false"});
    }
    void on_number(string_view key, string_view raw) override {
        events.push_back({"number", std::string(key), std::string(raw)});
    }
    void on_string(string_view key, string_view value) override {
        events.push_back({"string", std::string(key), std::string(value)});
    }
    void on_object_begin(string_view key) override {
        events.push_back({"object_begin", std::string(key), ""});
    }
    void on_object_end(string_view key) override {
        events.push_back({"object_end", std::string(key), ""});
    }
    void on_array_begin(string_view key) override {
        events.push_back({"array_begin", std::string(key), ""});
    }
    void on_array_end(string_view key) override {
        events.push_back({"array_end", std::string(key), ""});
    }
};

// ---------------------------------------------------------------------------
// Simple dispatch sink (like generated code would look)
// ---------------------------------------------------------------------------
struct CardVersionSink : JsonSink {
    std::string version;
    std::string device;
    std::string board;
    bool cell = false;
    bool wifi = false;
    std::string err;

    void on_string(string_view key, string_view val) override {
        if (key == "version") version = std::string(val);
        else if (key == "device") device = std::string(val);
        else if (key == "board") board = std::string(val);
        else if (key == "err") err = std::string(val);
    }
    void on_bool(string_view key, bool val) override {
        if (key == "cell") cell = val;
        else if (key == "wifi") wifi = val;
    }
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_CASE("buffer/sax_parser/empty_object") {
    RecordingSink sink;
    auto err = sax_parse("{}", sink);
    CHECK(err.empty());
    CHECK(sink.events.size() == 2);
    CHECK(sink.events[0].type == "object_begin");
    CHECK(sink.events[1].type == "object_end");
}

TEST_CASE("buffer/sax_parser/simple_object") {
    RecordingSink sink;
    auto err = sax_parse(R"({"req":"hub.set","mode":"periodic"})", sink);
    CHECK(err.empty());
    CHECK(sink.events.size() == 4);
    CHECK(sink.events[1].type == "string");
    CHECK(sink.events[1].key == "req");
    CHECK(sink.events[1].value == "hub.set");
    CHECK(sink.events[2].key == "mode");
    CHECK(sink.events[2].value == "periodic");
}

TEST_CASE("buffer/sax_parser/all_types") {
    RecordingSink sink;
    auto err = sax_parse(R"({"s":"hello","n":42,"f":3.14,"b":true,"z":false,"x":null})", sink);
    CHECK(err.empty());
    // object_begin, 6 values, object_end = 8 events
    CHECK(sink.events.size() == 8);
    CHECK(sink.events[1].type == "string"); CHECK(sink.events[1].value == "hello");
    CHECK(sink.events[2].type == "number"); CHECK(sink.events[2].value == "42");
    CHECK(sink.events[3].type == "number"); CHECK(sink.events[3].value == "3.14");
    CHECK(sink.events[4].type == "bool");   CHECK(sink.events[4].value == "true");
    CHECK(sink.events[5].type == "bool");   CHECK(sink.events[5].value == "false");
    CHECK(sink.events[6].type == "null");
}

TEST_CASE("buffer/sax_parser/nested_object") {
    RecordingSink sink;
    auto err = sax_parse(R"({"req":"note.add","body":{"temp":22.5,"label":"room-1"}})", sink);
    CHECK(err.empty());
    // root_begin, "req", body_begin, "temp", "label", body_end, root_end
    CHECK(sink.events[0].type == "object_begin"); CHECK(sink.events[0].key.empty());
    CHECK(sink.events[2].type == "object_begin"); CHECK(sink.events[2].key == "body");
    CHECK(sink.events[3].type == "number");        CHECK(sink.events[3].key == "temp");
    CHECK(sink.events[4].type == "string");        CHECK(sink.events[4].key == "label");
    CHECK(sink.events[5].type == "object_end");    CHECK(sink.events[5].key == "body");
    CHECK(sink.events[6].type == "object_end");    CHECK(sink.events[6].key.empty());
}

TEST_CASE("buffer/sax_parser/array") {
    RecordingSink sink;
    auto err = sax_parse(R"({"tags":["a","b","c"]})", sink);
    CHECK(err.empty());
    // root_begin, array_begin, "a", "b", "c", array_end, root_end
    CHECK(sink.events[1].type == "array_begin"); CHECK(sink.events[1].key == "tags");
    CHECK(sink.events[2].type == "string");       CHECK(sink.events[2].value == "a");
    CHECK(sink.events[3].type == "string");       CHECK(sink.events[3].value == "b");
    CHECK(sink.events[4].type == "string");       CHECK(sink.events[4].value == "c");
    CHECK(sink.events[5].type == "array_end");    CHECK(sink.events[5].key == "tags");
}

TEST_CASE("buffer/sax_parser/escape_sequences") {
    RecordingSink sink;
    auto err = sax_parse(R"({"msg":"hello \"world\"\nnewline\ttab\\back"})", sink);
    CHECK(err.empty());
    CHECK(sink.events[1].type == "string");
    CHECK(sink.events[1].value == "hello \"world\"\nnewline\ttab\\back");
}

TEST_CASE("buffer/sax_parser/unicode_escape") {
    RecordingSink sink;
    auto err = sax_parse("{\"c\":\"\\u0041\"}", sink);  // A = 'A'
    CHECK(err.empty());
    CHECK(sink.events[1].value == "A");
}

TEST_CASE("buffer/sax_parser/unicode_2byte") {
    RecordingSink sink;
    auto err = sax_parse("{\"c\":\"\\u00E9\"}", sink);  // é = 'e' with acute
    CHECK(err.empty());
    CHECK(sink.events[1].value.size() == 2);  // 2-byte UTF-8
    CHECK(sink.events[1].value == "\xC3\xA9");
}

TEST_CASE("buffer/sax_parser/numbers") {
    RecordingSink sink;
    auto err = sax_parse(R"({"a":0,"b":-1,"c":3.14,"d":1e10,"e":2.5E-3,"f":-0})", sink);
    CHECK(err.empty());
    CHECK(sink.events[1].value == "0");
    CHECK(sink.events[2].value == "-1");
    CHECK(sink.events[3].value == "3.14");
    CHECK(sink.events[4].value == "1e10");
    CHECK(sink.events[5].value == "2.5E-3");
    CHECK(sink.events[6].value == "-0");
}

TEST_CASE("buffer/sax_parser/card_version_sink") {
    CardVersionSink sink;
    auto err = sax_parse(
        R"({"version":"notecard-7.2.1","device":"dev:1234","board":"esp32","cell":true,"wifi":false})",
        sink);
    CHECK(err.empty());
    CHECK(sink.version == "notecard-7.2.1");
    CHECK(sink.device == "dev:1234");
    CHECK(sink.board == "esp32");
    CHECK(sink.cell == true);
    CHECK(sink.wifi == false);
}

TEST_CASE("buffer/sax_parser/notecard_error") {
    CardVersionSink sink;
    auto err = sax_parse(R"({"err":"not found {io}"})", sink);
    CHECK(err.empty());
    CHECK(sink.err == "not found {io}");
}

TEST_CASE("buffer/sax_parser/whitespace") {
    RecordingSink sink;
    auto err = sax_parse("  { \"a\" : 1 , \"b\" : 2 }  ", sink);
    CHECK(err.empty());
    CHECK(sink.events.size() == 4);
}

TEST_CASE("buffer/sax_parser/reject_invalid") {
    RecordingSink sink;

    // Not JSON
    CHECK(!sax_parse("not json", sink).empty());

    // Trailing garbage
    CHECK(!sax_parse("{} garbage", sink).empty());

    // Trailing comma
    CHECK(!sax_parse(R"({"a":1,})", sink).empty());

    // Unquoted key
    CHECK(!sax_parse(R"({a:1})", sink).empty());

    // Single quotes
    CHECK(!sax_parse("{'a':1}", sink).empty());

    // Leading zero
    CHECK(!sax_parse(R"({"a":01})", sink).empty());

    // NaN/Infinity
    CHECK(!sax_parse(R"({"a":NaN})", sink).empty());
    CHECK(!sax_parse(R"({"a":Infinity})", sink).empty());
}

TEST_CASE("buffer/sax_parser/empty_string_value") {
    RecordingSink sink;
    auto err = sax_parse(R"({"key":""})", sink);
    CHECK(err.empty());
    CHECK(sink.events[1].value.empty());
}

TEST_CASE("buffer/sax_parser/deeply_nested") {
    RecordingSink sink;
    auto err = sax_parse(R"({"a":{"b":{"c":{"d":"deep"}}}})", sink);
    CHECK(err.empty());
    // Find the "deep" value
    bool found = false;
    for (auto& e : sink.events) {
        if (e.type == "string" && e.key == "d" && e.value == "deep") found = true;
    }
    CHECK(found);
}

TEST_CASE("buffer/sax_parser/empty_input") {
    RecordingSink sink;
    CHECK(!sax_parse("", sink).empty());
    CHECK(!sax_parse("   ", sink).empty());
}

TEST_CASE("buffer/sax_parser/all_escape_types") {
    RecordingSink sink;
    // Test \/, \b, \f, \r which weren't covered by escape_sequences
    auto err = sax_parse(R"({"a":"slash\/here","b":"back\bspace","c":"form\ffeed","d":"cr\rret"})", sink);
    CHECK(err.empty());
    CHECK(sink.events[1].value == "slash/here");
    CHECK(sink.events[2].value == "back\bspace");
    CHECK(sink.events[3].value == "form\ffeed");
    CHECK(sink.events[4].value == "cr\rret");
}

TEST_CASE("buffer/sax_parser/unicode_3byte") {
    RecordingSink sink;
    // 世 = '世' (3-byte UTF-8: E4 B8 96)
    auto err = sax_parse("{\"c\":\"\\u4E16\"}", sink);
    CHECK(err.empty());
    CHECK(sink.events[1].value.size() == 3);
    CHECK(sink.events[1].value == "\xE4\xB8\x96");
}

TEST_CASE("buffer/sax_parser/unicode_surrogate") {
    RecordingSink sink;
    // Lone surrogate \uD800 — should produce '?'
    auto err = sax_parse(R"({"c":"\uD800"})", sink);
    CHECK(err.empty());
    CHECK(sink.events[1].value == "?");
}

TEST_CASE("buffer/sax_parser/empty_array") {
    RecordingSink sink;
    auto err = sax_parse(R"({"tags":[]})", sink);
    CHECK(err.empty());
    CHECK(sink.events[1].type == "array_begin");
    CHECK(sink.events[1].key == "tags");
    CHECK(sink.events[2].type == "array_end");
    CHECK(sink.events[2].key == "tags");
}

TEST_CASE("buffer/sax_parser/array_mixed_types") {
    RecordingSink sink;
    auto err = sax_parse(R"({"a":[1,true,null,"hi"]})", sink);
    CHECK(err.empty());
    // array_begin, number(1), bool(true), null, string(hi), array_end
    CHECK(sink.events[1].type == "array_begin");
    CHECK(sink.events[2].type == "number"); CHECK(sink.events[2].value == "1");
    CHECK(sink.events[3].type == "bool");   CHECK(sink.events[3].value == "true");
    CHECK(sink.events[4].type == "null");
    CHECK(sink.events[5].type == "string"); CHECK(sink.events[5].value == "hi");
    CHECK(sink.events[6].type == "array_end");
}

// ---------------------------------------------------------------------------
// Error path tests — ensure every parser error is reachable.
// ---------------------------------------------------------------------------
TEST_CASE("buffer/sax_parser/error_unterminated_string") {
    RecordingSink sink;
    auto err = sax_parse(R"({"a":"no close)", sink);
    CHECK(!err.empty());
}

TEST_CASE("buffer/sax_parser/error_unexpected_end_in_escape") {
    RecordingSink sink;
    std::string json = R"({"a":"trail\)";
    auto err = sax_parse(json.data(), json.size(), sink);
    CHECK(!err.empty());
}

TEST_CASE("buffer/sax_parser/error_invalid_hex_escape") {
    RecordingSink sink;
    auto err = sax_parse(R"({"a":"\uZZZZ"})", sink);
    CHECK(!err.empty());
}

TEST_CASE("buffer/sax_parser/error_unescaped_control_char") {
    RecordingSink sink;
    // String with raw 0x01 byte
    std::string json = "{\"a\":\"x\x01y\"}";
    auto err = sax_parse(json.data(), json.size(), sink);
    CHECK(!err.empty());
}

TEST_CASE("buffer/sax_parser/error_missing_colon") {
    RecordingSink sink;
    auto err = sax_parse(R"({"a" "b"})", sink);
    CHECK(!err.empty());
}

TEST_CASE("buffer/sax_parser/error_missing_comma_object") {
    RecordingSink sink;
    auto err = sax_parse(R"({"a":1 "b":2})", sink);
    CHECK(!err.empty());
}

TEST_CASE("buffer/sax_parser/error_missing_comma_array") {
    RecordingSink sink;
    auto err = sax_parse(R"({"a":[1 2]})", sink);
    CHECK(!err.empty());
}

TEST_CASE("buffer/sax_parser/error_truncated_true") {
    RecordingSink sink;
    auto err = sax_parse(R"({"a":tru})", sink);
    CHECK(!err.empty());
}

TEST_CASE("buffer/sax_parser/error_invalid_literal") {
    RecordingSink sink;
    auto err = sax_parse(R"({"a":tRue})", sink);
    CHECK(!err.empty());
}

TEST_CASE("buffer/sax_parser/error_truncated_null") {
    RecordingSink sink;
    auto err = sax_parse(R"({"a":nul})", sink);
    CHECK(!err.empty());
}

TEST_CASE("buffer/sax_parser/error_bare_minus") {
    RecordingSink sink;
    auto err = sax_parse(R"({"a":-})", sink);
    CHECK(!err.empty());
}

TEST_CASE("buffer/sax_parser/error_trailing_dot") {
    RecordingSink sink;
    auto err = sax_parse(R"({"a":1.})", sink);
    CHECK(!err.empty());
}

TEST_CASE("buffer/sax_parser/error_bare_exponent") {
    RecordingSink sink;
    auto err = sax_parse(R"({"a":1e})", sink);
    CHECK(!err.empty());
}

TEST_CASE("buffer/sax_parser/error_truncated_false") {
    RecordingSink sink;
    // "fals" at end of input — triggers unexpected end in literal
    std::string json = R"({"a":fals)";
    auto err = sax_parse(json.data(), json.size(), sink);
    CHECK(!err.empty());
}

TEST_CASE("buffer/sax_parser/error_number_end_of_input") {
    RecordingSink sink;
    // Negative sign at end of input
    std::string json = R"({"a":-)";
    auto err = sax_parse(json.data(), json.size(), sink);
    CHECK(!err.empty());
}
