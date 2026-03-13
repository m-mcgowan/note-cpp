// Integration tests for the SAX JSON parser.

#include <note/json_sax.hpp>

#include <cassert>
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

static void test_empty_object() {
    RecordingSink sink;
    auto err = sax_parse("{}", sink);
    assert(err.empty());
    assert(sink.events.size() == 2);
    assert(sink.events[0].type == "object_begin");
    assert(sink.events[1].type == "object_end");
    std::puts("  PASS: empty_object");
}

static void test_simple_object() {
    RecordingSink sink;
    auto err = sax_parse(R"({"req":"hub.set","mode":"periodic"})", sink);
    assert(err.empty());
    assert(sink.events.size() == 4);
    assert(sink.events[1].type == "string");
    assert(sink.events[1].key == "req");
    assert(sink.events[1].value == "hub.set");
    assert(sink.events[2].key == "mode");
    assert(sink.events[2].value == "periodic");
    std::puts("  PASS: simple_object");
}

static void test_all_types() {
    RecordingSink sink;
    auto err = sax_parse(R"({"s":"hello","n":42,"f":3.14,"b":true,"z":false,"x":null})", sink);
    assert(err.empty());
    // object_begin, 6 values, object_end = 8 events
    assert(sink.events.size() == 8);
    assert(sink.events[1].type == "string" && sink.events[1].value == "hello");
    assert(sink.events[2].type == "number" && sink.events[2].value == "42");
    assert(sink.events[3].type == "number" && sink.events[3].value == "3.14");
    assert(sink.events[4].type == "bool" && sink.events[4].value == "true");
    assert(sink.events[5].type == "bool" && sink.events[5].value == "false");
    assert(sink.events[6].type == "null");
    std::puts("  PASS: all_types");
}

static void test_nested_object() {
    RecordingSink sink;
    auto err = sax_parse(R"({"req":"note.add","body":{"temp":22.5,"label":"room-1"}})", sink);
    assert(err.empty());
    // root_begin, "req", body_begin, "temp", "label", body_end, root_end
    assert(sink.events[0].type == "object_begin" && sink.events[0].key.empty());
    assert(sink.events[2].type == "object_begin" && sink.events[2].key == "body");
    assert(sink.events[3].type == "number" && sink.events[3].key == "temp");
    assert(sink.events[4].type == "string" && sink.events[4].key == "label");
    assert(sink.events[5].type == "object_end" && sink.events[5].key == "body");
    assert(sink.events[6].type == "object_end" && sink.events[6].key.empty());
    std::puts("  PASS: nested_object");
}

static void test_array() {
    RecordingSink sink;
    auto err = sax_parse(R"({"tags":["a","b","c"]})", sink);
    assert(err.empty());
    // root_begin, array_begin, "a", "b", "c", array_end, root_end
    assert(sink.events[1].type == "array_begin" && sink.events[1].key == "tags");
    assert(sink.events[2].type == "string" && sink.events[2].value == "a");
    assert(sink.events[3].type == "string" && sink.events[3].value == "b");
    assert(sink.events[4].type == "string" && sink.events[4].value == "c");
    assert(sink.events[5].type == "array_end" && sink.events[5].key == "tags");
    std::puts("  PASS: array");
}

static void test_escape_sequences() {
    RecordingSink sink;
    auto err = sax_parse(R"({"msg":"hello \"world\"\nnewline\ttab\\back"})", sink);
    assert(err.empty());
    assert(sink.events[1].type == "string");
    assert(sink.events[1].value == "hello \"world\"\nnewline\ttab\\back");
    std::puts("  PASS: escape_sequences");
}

static void test_unicode_escape() {
    RecordingSink sink;
    auto err = sax_parse(R"({"c":"\u0041"})", sink);  // \u0041 = 'A'
    assert(err.empty());
    assert(sink.events[1].value == "A");
    std::puts("  PASS: unicode_escape");
}

static void test_unicode_2byte() {
    RecordingSink sink;
    auto err = sax_parse(R"({"c":"\u00E9"})", sink);  // \u00E9 = 'e' with acute
    assert(err.empty());
    assert(sink.events[1].value.size() == 2);  // 2-byte UTF-8
    assert(sink.events[1].value == "\xC3\xA9");
    std::puts("  PASS: unicode_2byte");
}

static void test_numbers() {
    RecordingSink sink;
    auto err = sax_parse(R"({"a":0,"b":-1,"c":3.14,"d":1e10,"e":2.5E-3,"f":-0})", sink);
    assert(err.empty());
    assert(sink.events[1].value == "0");
    assert(sink.events[2].value == "-1");
    assert(sink.events[3].value == "3.14");
    assert(sink.events[4].value == "1e10");
    assert(sink.events[5].value == "2.5E-3");
    assert(sink.events[6].value == "-0");
    std::puts("  PASS: numbers");
}

static void test_card_version_sink() {
    CardVersionSink sink;
    auto err = sax_parse(
        R"({"version":"notecard-7.2.1","device":"dev:1234","board":"esp32","cell":true,"wifi":false})",
        sink);
    assert(err.empty());
    assert(sink.version == "notecard-7.2.1");
    assert(sink.device == "dev:1234");
    assert(sink.board == "esp32");
    assert(sink.cell == true);
    assert(sink.wifi == false);
    std::puts("  PASS: card_version_sink");
}

static void test_notecard_error() {
    CardVersionSink sink;
    auto err = sax_parse(R"({"err":"not found {io}"})", sink);
    assert(err.empty());
    assert(sink.err == "not found {io}");
    std::puts("  PASS: notecard_error");
}

static void test_whitespace() {
    RecordingSink sink;
    auto err = sax_parse("  { \"a\" : 1 , \"b\" : 2 }  ", sink);
    assert(err.empty());
    assert(sink.events.size() == 4);
    std::puts("  PASS: whitespace");
}

static void test_reject_invalid() {
    RecordingSink sink;

    // Not JSON
    assert(!sax_parse("not json", sink).empty());

    // Trailing garbage
    assert(!sax_parse("{} garbage", sink).empty());

    // Trailing comma
    assert(!sax_parse(R"({"a":1,})", sink).empty());

    // Unquoted key
    assert(!sax_parse(R"({a:1})", sink).empty());

    // Single quotes
    assert(!sax_parse("{'a':1}", sink).empty());

    // Leading zero
    assert(!sax_parse(R"({"a":01})", sink).empty());

    // NaN/Infinity
    assert(!sax_parse(R"({"a":NaN})", sink).empty());
    assert(!sax_parse(R"({"a":Infinity})", sink).empty());

    std::puts("  PASS: reject_invalid");
}

static void test_empty_string_value() {
    RecordingSink sink;
    auto err = sax_parse(R"({"key":""})", sink);
    assert(err.empty());
    assert(sink.events[1].value.empty());
    std::puts("  PASS: empty_string_value");
}

static void test_deeply_nested() {
    RecordingSink sink;
    auto err = sax_parse(R"({"a":{"b":{"c":{"d":"deep"}}}})", sink);
    assert(err.empty());
    // Find the "deep" value
    bool found = false;
    for (auto& e : sink.events) {
        if (e.type == "string" && e.key == "d" && e.value == "deep") found = true;
    }
    assert(found);
    std::puts("  PASS: deeply_nested");
}

static void test_empty_input() {
    RecordingSink sink;
    assert(!sax_parse("", sink).empty());
    assert(!sax_parse("   ", sink).empty());
    std::puts("  PASS: empty_input");
}

static void test_all_escape_types() {
    RecordingSink sink;
    // Test \/, \b, \f, \r which weren't covered by test_escape_sequences
    auto err = sax_parse(R"({"a":"slash\/here","b":"back\bspace","c":"form\ffeed","d":"cr\rret"})", sink);
    assert(err.empty());
    assert(sink.events[1].value == "slash/here");
    assert(sink.events[2].value == "back\bspace");
    assert(sink.events[3].value == "form\ffeed");
    assert(sink.events[4].value == "cr\rret");
    std::puts("  PASS: all_escape_types");
}

static void test_unicode_3byte() {
    RecordingSink sink;
    // \u4E16 = '世' (3-byte UTF-8: E4 B8 96)
    auto err = sax_parse(R"({"c":"\u4E16"})", sink);
    assert(err.empty());
    assert(sink.events[1].value.size() == 3);
    assert(sink.events[1].value == "\xE4\xB8\x96");
    std::puts("  PASS: unicode_3byte");
}

static void test_unicode_surrogate() {
    RecordingSink sink;
    // Lone surrogate \uD800 — should produce '?'
    auto err = sax_parse(R"({"c":"\uD800"})", sink);
    assert(err.empty());
    assert(sink.events[1].value == "?");
    std::puts("  PASS: unicode_surrogate");
}

static void test_empty_array() {
    RecordingSink sink;
    auto err = sax_parse(R"({"tags":[]})", sink);
    assert(err.empty());
    assert(sink.events[1].type == "array_begin");
    assert(sink.events[1].key == "tags");
    assert(sink.events[2].type == "array_end");
    assert(sink.events[2].key == "tags");
    std::puts("  PASS: empty_array");
}

static void test_array_mixed_types() {
    RecordingSink sink;
    auto err = sax_parse(R"({"a":[1,true,null,"hi"]})", sink);
    assert(err.empty());
    // array_begin, number(1), bool(true), null, string(hi), array_end
    assert(sink.events[1].type == "array_begin");
    assert(sink.events[2].type == "number" && sink.events[2].value == "1");
    assert(sink.events[3].type == "bool" && sink.events[3].value == "true");
    assert(sink.events[4].type == "null");
    assert(sink.events[5].type == "string" && sink.events[5].value == "hi");
    assert(sink.events[6].type == "array_end");
    std::puts("  PASS: array_mixed_types");
}

// ---------------------------------------------------------------------------
// Error path tests — ensure every parser error is reachable.
// ---------------------------------------------------------------------------
static void test_error_unterminated_string() {
    RecordingSink sink;
    auto err = sax_parse(R"({"a":"no close)", sink);
    assert(!err.empty());
    std::puts("  PASS: error_unterminated_string");
}

static void test_error_unexpected_end_in_escape() {
    RecordingSink sink;
    std::string json = R"({"a":"trail\)";
    auto err = sax_parse(json.data(), json.size(), sink);
    assert(!err.empty());
    std::puts("  PASS: error_unexpected_end_in_escape");
}

static void test_error_invalid_hex_escape() {
    RecordingSink sink;
    auto err = sax_parse(R"({"a":"\uZZZZ"})", sink);
    assert(!err.empty());
    std::puts("  PASS: error_invalid_hex_escape");
}

static void test_error_unescaped_control_char() {
    RecordingSink sink;
    // String with raw 0x01 byte
    std::string json = "{\"a\":\"x\x01y\"}";
    auto err = sax_parse(json.data(), json.size(), sink);
    assert(!err.empty());
    std::puts("  PASS: error_unescaped_control_char");
}

static void test_error_missing_colon() {
    RecordingSink sink;
    auto err = sax_parse(R"({"a" "b"})", sink);
    assert(!err.empty());
    std::puts("  PASS: error_missing_colon");
}

static void test_error_missing_comma_object() {
    RecordingSink sink;
    auto err = sax_parse(R"({"a":1 "b":2})", sink);
    assert(!err.empty());
    std::puts("  PASS: error_missing_comma_object");
}

static void test_error_missing_comma_array() {
    RecordingSink sink;
    auto err = sax_parse(R"({"a":[1 2]})", sink);
    assert(!err.empty());
    std::puts("  PASS: error_missing_comma_array");
}

static void test_error_truncated_true() {
    RecordingSink sink;
    auto err = sax_parse(R"({"a":tru})", sink);
    assert(!err.empty());
    std::puts("  PASS: error_truncated_true");
}

static void test_error_invalid_literal() {
    RecordingSink sink;
    auto err = sax_parse(R"({"a":tRue})", sink);
    assert(!err.empty());
    std::puts("  PASS: error_invalid_literal");
}

static void test_error_truncated_null() {
    RecordingSink sink;
    auto err = sax_parse(R"({"a":nul})", sink);
    assert(!err.empty());
    std::puts("  PASS: error_truncated_null");
}

static void test_error_bare_minus() {
    RecordingSink sink;
    auto err = sax_parse(R"({"a":-})", sink);
    assert(!err.empty());
    std::puts("  PASS: error_bare_minus");
}

static void test_error_trailing_dot() {
    RecordingSink sink;
    auto err = sax_parse(R"({"a":1.})", sink);
    assert(!err.empty());
    std::puts("  PASS: error_trailing_dot");
}

static void test_error_bare_exponent() {
    RecordingSink sink;
    auto err = sax_parse(R"({"a":1e})", sink);
    assert(!err.empty());
    std::puts("  PASS: error_bare_exponent");
}

static void test_error_truncated_false() {
    RecordingSink sink;
    // "fals" at end of input — triggers unexpected end in literal
    std::string json = R"({"a":fals)";
    auto err = sax_parse(json.data(), json.size(), sink);
    assert(!err.empty());
    std::puts("  PASS: error_truncated_false");
}

static void test_error_number_end_of_input() {
    RecordingSink sink;
    // Negative sign at end of input
    std::string json = R"({"a":-)";
    auto err = sax_parse(json.data(), json.size(), sink);
    assert(!err.empty());
    std::puts("  PASS: error_number_end_of_input");
}

// ---------------------------------------------------------------------------
int main() {
    std::puts("=== SAX parser integration tests ===");
    test_empty_object();
    test_simple_object();
    test_all_types();
    test_nested_object();
    test_array();
    test_escape_sequences();
    test_unicode_escape();
    test_unicode_2byte();
    test_numbers();
    test_card_version_sink();
    test_notecard_error();
    test_whitespace();
    test_reject_invalid();
    test_empty_string_value();
    test_deeply_nested();
    test_empty_input();
    test_all_escape_types();
    test_unicode_3byte();
    test_unicode_surrogate();
    test_empty_array();
    test_array_mixed_types();
    test_error_unterminated_string();
    test_error_unexpected_end_in_escape();
    test_error_invalid_hex_escape();
    test_error_unescaped_control_char();
    test_error_missing_colon();
    test_error_missing_comma_object();
    test_error_missing_comma_array();
    test_error_truncated_true();
    test_error_invalid_literal();
    test_error_truncated_null();
    test_error_bare_minus();
    test_error_trailing_dot();
    test_error_bare_exponent();
    test_error_truncated_false();
    test_error_number_end_of_input();
    std::puts("\nAll SAX parser tests passed.");
    return 0;
}
