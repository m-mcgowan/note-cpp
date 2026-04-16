// Tests for the zero-buffer JSON lexer and its strategies.
//
// Test cases mirror the existing sax_parse and streaming sax tests to ensure
// the lexer accepts the same valid JSON and rejects the same invalid JSON.

#include "catch.hpp"

#include <note/lexer/json_lexer.hpp>
#include <note/lexer/parse.hpp>
#include <note/json_sax.hpp>

#include <cstring>
#include <string>
#include <vector>

using namespace note;

// ── Helpers ──────────────────────────────────────────────────────────────

static std::vector<LexerEvent> lex(const char* json) {
    std::vector<LexerEvent> events;
    DefaultLexer lexer;
    for (const char* p = json; *p; ++p) {
        lexer.feed(static_cast<uint8_t>(*p), [&](LexerEvent ev) {
            events.push_back(ev);
        });
        if (lexer.has_error()) break;
    }
    return events;
}

static bool lex_ok(const char* json) {
    auto events = lex(json);
    for (auto& ev : events)
        if (ev.tag == LexerEvent::Error) return false;
    return true;
}

/// Returns true if the input produces an error event OR is incomplete
/// (lexer not in done state after all input consumed).
static bool lex_error(const char* json) {
    DefaultLexer lexer;
    bool got_error = false;
    for (const char* p = json; *p; ++p) {
        lexer.feed(static_cast<uint8_t>(*p), [&](LexerEvent ev) {
            if (ev.tag == LexerEvent::Error) got_error = true;
        });
        if (got_error) return true;
    }
    // Incomplete input (e.g. unterminated string) is also an error
    return !lexer.is_done();
}

static std::string collect_chars(const std::vector<LexerEvent>& events,
                                  LexerEvent::Tag tag) {
    std::string out;
    for (auto& ev : events)
        if (ev.tag == tag) out += ev.ch;
    return out;
}

static std::vector<std::string> collect_keys(const std::vector<LexerEvent>& events) {
    std::vector<std::string> keys;
    std::string current;
    for (auto& ev : events) {
        if (ev.tag == LexerEvent::KeyChar) current += ev.ch;
        else if (ev.tag == LexerEvent::KeyEnd) {
            keys.push_back(current);
            current.clear();
        }
    }
    return keys;
}

template<typename T>
static std::vector<T> collect_values(const std::vector<LexerEvent>& events,
                                      LexerEvent::Tag tag) {
    std::vector<T> out;
    for (auto& ev : events) {
        if (ev.tag == tag) {
            if constexpr (std::is_same_v<T, int32_t>) out.push_back(ev.integer);
            else if constexpr (std::is_same_v<T, double>) out.push_back(ev.floating);
            else if constexpr (std::is_same_v<T, bool>) out.push_back(ev.boolean);
        }
    }
    return out;
}

// ═════════════════════════════════════════════════════════════════════════
// BitStack
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE("BitStack: push and pop objects") {
    BitStack<uint32_t> s;
    REQUIRE(s.empty());
    REQUIRE(s.push_object());
    REQUIRE(s.in_object());
    REQUIRE_FALSE(s.in_array());
    REQUIRE(s.pop_object());
    REQUIRE(s.empty());
}

TEST_CASE("BitStack: push and pop arrays") {
    BitStack<uint32_t> s;
    REQUIRE(s.push_array());
    REQUIRE(s.in_array());
    REQUIRE_FALSE(s.in_object());
    REQUIRE(s.pop_array());
    REQUIRE(s.empty());
}

TEST_CASE("BitStack: mismatch — push object, pop array") {
    BitStack<uint32_t> s;
    s.push_object();
    REQUIRE_FALSE(s.pop_array());
}

TEST_CASE("BitStack: mismatch — push array, pop object") {
    BitStack<uint32_t> s;
    s.push_array();
    REQUIRE_FALSE(s.pop_object());
}

TEST_CASE("BitStack: nested object in array in object") {
    BitStack<uint32_t> s;
    REQUIRE(s.push_object());
    REQUIRE(s.push_array());
    REQUIRE(s.push_object());
    REQUIRE(s.in_object());
    REQUIRE(s.pop_object());
    REQUIRE(s.in_array());
    REQUIRE(s.pop_array());
    REQUIRE(s.in_object());
    REQUIRE(s.pop_object());
    REQUIRE(s.empty());
}

TEST_CASE("BitStack: overflow at max depth") {
    BitStack<uint8_t> s;  // 8 levels max
    for (int i = 0; i < 8; ++i) REQUIRE(s.push_object());
    REQUIRE_FALSE(s.push_object());
    REQUIRE_FALSE(s.push_array());
}

TEST_CASE("BitStack: pop on empty") {
    BitStack<uint32_t> s;
    REQUIRE_FALSE(s.pop_object());
    REQUIRE_FALSE(s.pop_array());
}

TEST_CASE("BitStack: alternating types") {
    BitStack<uint32_t> s;
    s.push_object();
    s.push_array();
    s.push_object();
    s.push_array();
    REQUIRE(s.pop_array());
    REQUIRE(s.pop_object());
    REQUIRE(s.pop_array());
    REQUIRE(s.pop_object());
    REQUIRE(s.empty());
}

// ═════════════════════════════════════════════════════════════════════════
// IncrementalNumber
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE("Number: positive integer") {
    IncrementalNumber n;
    n.add_digit(4); n.add_digit(2);
    REQUIRE(n.is_integer());
    REQUIRE(n.to_int32() == 42);
}

TEST_CASE("Number: negative integer") {
    IncrementalNumber n;
    n.set_negative();
    n.add_digit(7);
    REQUIRE(n.to_int32() == -7);
}

TEST_CASE("Number: zero") {
    IncrementalNumber n;
    n.add_digit(0);
    REQUIRE(n.is_integer());
    REQUIRE(n.to_int32() == 0);
}

TEST_CASE("Number: negative zero") {
    IncrementalNumber n;
    n.set_negative();
    n.add_digit(0);
    REQUIRE(n.to_int32() == 0);
}

TEST_CASE("Number: large integer near int32 max") {
    IncrementalNumber n;
    // 2147483647 = INT32_MAX
    for (char c : std::string("2147483647"))
        n.add_digit(static_cast<uint8_t>(c - '0'));
    REQUIRE(n.is_integer());
    REQUIRE(n.to_int32() == 2147483647);
}

TEST_CASE("Number: float with fraction") {
    IncrementalNumber n;
    n.add_digit(3);
    n.start_fraction();
    n.add_frac_digit(1); n.add_frac_digit(4);
    REQUIRE_FALSE(n.is_integer());
    REQUIRE(n.to_float() == Approx(3.14));
}

TEST_CASE("Number: negative float") {
    IncrementalNumber n;
    n.set_negative();
    n.add_digit(2);
    n.start_fraction();
    n.add_frac_digit(5);
    REQUIRE(n.to_float() == Approx(-2.5));
}

TEST_CASE("Number: zero point five") {
    IncrementalNumber n;
    n.add_digit(0);
    n.start_fraction();
    n.add_frac_digit(5);
    REQUIRE(n.to_float() == Approx(0.5));
}

TEST_CASE("Number: many fractional digits") {
    IncrementalNumber n;
    n.add_digit(1);
    n.start_fraction();
    // 0.123456789
    for (int d = 1; d <= 9; ++d) n.add_frac_digit(static_cast<uint8_t>(d));
    REQUIRE(n.to_float() == Approx(1.123456789));
}

TEST_CASE("Number: positive exponent") {
    IncrementalNumber n;
    n.add_digit(1);
    n.start_fraction();
    n.add_frac_digit(5);
    n.start_exponent();
    n.add_exp_digit(3);
    REQUIRE(n.to_float() == Approx(1500.0));
}

TEST_CASE("Number: negative exponent") {
    IncrementalNumber n;
    n.add_digit(5);
    n.start_exponent();
    n.set_exp_negative();
    n.add_exp_digit(2);
    REQUIRE(n.to_float() == Approx(0.05));
}

TEST_CASE("Number: integer with exponent becomes float") {
    IncrementalNumber n;
    n.add_digit(1);
    n.start_exponent();
    n.add_exp_digit(2);
    REQUIRE_FALSE(n.is_integer());
    REQUIRE(n.to_float() == Approx(100.0));
}

TEST_CASE("Number: reset clears state") {
    IncrementalNumber n;
    n.set_negative();
    n.add_digit(5);
    n.start_fraction();
    n.add_frac_digit(3);
    n.reset();
    n.add_digit(1);
    REQUIRE(n.is_integer());
    REQUIRE(n.to_int32() == 1);
}

// ═════════════════════════════════════════════════════════════════════════
// Utf8EscapeDecoder
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE("Escape: simple escapes") {
    Utf8EscapeDecoder d;
    auto test = [&](char input, char expected) {
        std::string out;
        REQUIRE(d.feed(input, [&](char c) { out += c; }));
        REQUIRE(out.size() == 1);
        REQUIRE(out[0] == expected);
    };
    test('"', '"');
    test('\\', '\\');
    test('/', '/');
    test('b', '\b');
    test('f', '\f');
    test('n', '\n');
    test('r', '\r');
    test('t', '\t');
}

TEST_CASE("Escape: invalid escape character") {
    Utf8EscapeDecoder d;
    REQUIRE_FALSE(d.feed('x', [](char) {}));
    REQUIRE_FALSE(d.feed('a', [](char) {}));
    REQUIRE_FALSE(d.feed('1', [](char) {}));
}

TEST_CASE("Escape: \\u0041 → A (ASCII)") {
    Utf8EscapeDecoder d;
    std::string out;
    auto emit = [&](char c) { out += c; };
    d.feed('u', emit);
    REQUIRE(d.in_unicode());
    d.feed_hex('0', emit); d.feed_hex('0', emit);
    d.feed_hex('4', emit); d.feed_hex('1', emit);
    REQUIRE_FALSE(d.in_unicode());
    REQUIRE(out == "A");
}

TEST_CASE("Escape: \\u00E9 → 2-byte UTF-8 (é)") {
    Utf8EscapeDecoder d;
    std::string out;
    auto emit = [&](char c) { out += c; };
    d.feed('u', emit);
    d.feed_hex('0', emit); d.feed_hex('0', emit);
    d.feed_hex('E', emit); d.feed_hex('9', emit);
    REQUIRE(out.size() == 2);
    CHECK(out[0] == '\xC3');
    CHECK(out[1] == '\xA9');
}

TEST_CASE("Escape: \\u4E16 → 3-byte UTF-8 (世)") {
    Utf8EscapeDecoder d;
    std::string out;
    auto emit = [&](char c) { out += c; };
    d.feed('u', emit);
    d.feed_hex('4', emit); d.feed_hex('E', emit);
    d.feed_hex('1', emit); d.feed_hex('6', emit);
    REQUIRE(out.size() == 3);
}

TEST_CASE("Escape: surrogate → replacement char") {
    Utf8EscapeDecoder d;
    std::string out;
    auto emit = [&](char c) { out += c; };
    d.feed('u', emit);
    d.feed_hex('D', emit); d.feed_hex('8', emit);
    d.feed_hex('0', emit); d.feed_hex('0', emit);
    REQUIRE(out == "?");
}

TEST_CASE("Escape: invalid hex digit") {
    Utf8EscapeDecoder d;
    auto noop = [](char) {};
    d.feed('u', noop);
    REQUIRE_FALSE(d.feed_hex('G', noop));
}

TEST_CASE("Escape: lowercase hex") {
    Utf8EscapeDecoder d;
    std::string out;
    auto emit = [&](char c) { out += c; };
    d.feed('u', emit);
    d.feed_hex('0', emit); d.feed_hex('0', emit);
    d.feed_hex('6', emit); d.feed_hex('a', emit);  // 0x006A = 'j'
    REQUIRE(out == "j");
}

// ═════════════════════════════════════════════════════════════════════════
// JsonLexer: valid JSON (mirrors sax_parse / streaming sax tests)
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE("Lexer: empty object") {
    auto events = lex("{}");
    REQUIRE(events.size() == 2);
    CHECK(events[0].tag == LexerEvent::ObjectBegin);
    CHECK(events[1].tag == LexerEvent::ObjectEnd);
}

TEST_CASE("Lexer: empty array") {
    auto events = lex("[]");
    REQUIRE(events.size() == 2);
    CHECK(events[0].tag == LexerEvent::ArrayBegin);
    CHECK(events[1].tag == LexerEvent::ArrayEnd);
}

TEST_CASE("Lexer: string values") {
    REQUIRE(lex_ok(R"({"status":"connected","device":"dev:123"})"));
    auto events = lex(R"({"status":"connected"})");
    auto keys = collect_keys(events);
    REQUIRE(keys.size() == 1);
    CHECK(keys[0] == "status");
    CHECK(collect_chars(events, LexerEvent::StringChar) == "connected");
}

TEST_CASE("Lexer: number values") {
    auto events = lex(R"({"a":0,"b":3.14,"c":-1,"d":2.5e10})");
    auto ints = collect_values<int32_t>(events, LexerEvent::Integer);
    auto floats = collect_values<double>(events, LexerEvent::Float);
    REQUIRE(ints.size() == 2);
    CHECK(ints[0] == 0);
    CHECK(ints[1] == -1);
    REQUIRE(floats.size() == 2);
    CHECK(floats[0] == Approx(3.14));
    CHECK(floats[1] == Approx(2.5e10));
}

TEST_CASE("Lexer: booleans and null") {
    auto events = lex(R"({"ok":true,"n":null,"f":false})");
    auto bools = collect_values<bool>(events, LexerEvent::Bool);
    REQUIRE(bools.size() == 2);
    CHECK(bools[0] == true);
    CHECK(bools[1] == false);
    bool has_null = false;
    for (auto& ev : events) if (ev.tag == LexerEvent::Null) has_null = true;
    REQUIRE(has_null);
}

TEST_CASE("Lexer: nested object") {
    auto events = lex(R"({"inner":{"a":1},"b":"x"})");
    auto keys = collect_keys(events);
    REQUIRE(keys.size() == 3);
    CHECK(keys[0] == "inner");
    CHECK(keys[1] == "a");
    CHECK(keys[2] == "b");
}

TEST_CASE("Lexer: array of numbers") {
    auto events = lex(R"({"items":[1,2,3]})");
    auto ints = collect_values<int32_t>(events, LexerEvent::Integer);
    REQUIRE(ints.size() == 3);
    CHECK(ints[0] == 1); CHECK(ints[1] == 2); CHECK(ints[2] == 3);
}

TEST_CASE("Lexer: escaped strings") {
    auto events = lex(R"({"msg":"hello\nworld","path":"a\\b"})");
    // Collect string chars for each value
    std::vector<std::string> values;
    std::string current;
    for (auto& ev : events) {
        if (ev.tag == LexerEvent::StringChar) current += ev.ch;
        else if (ev.tag == LexerEvent::StringEnd) {
            values.push_back(current); current.clear();
        }
    }
    REQUIRE(values.size() == 2);
    CHECK(values[0] == "hello\nworld");
    CHECK(values[1] == "a\\b");
}

#if NOTE_UNICODE_ESCAPES
TEST_CASE("Lexer: unicode escape") {
    auto events = lex(R"({"c":"\u0041"})");
    CHECK(collect_chars(events, LexerEvent::StringChar) == "A");
}
#endif

TEST_CASE("Lexer: number with exponent sign") {
    REQUIRE(lex_ok(R"({"a":1e+2,"b":1e-2,"c":1E3})"));
    auto events = lex(R"({"a":1e+2})");
    auto floats = collect_values<double>(events, LexerEvent::Float);
    REQUIRE(floats.size() == 1);
    CHECK(floats[0] == Approx(100.0));
}

TEST_CASE("Lexer: leading zero number") {
    // 0 is valid, 0.5 is valid
    REQUIRE(lex_ok(R"({"z":0,"f":0.5})"));
    auto events = lex(R"({"z":0,"f":0.5})");
    auto ints = collect_values<int32_t>(events, LexerEvent::Integer);
    auto floats = collect_values<double>(events, LexerEvent::Float);
    CHECK(ints.size() == 1);
    CHECK(ints[0] == 0);
    CHECK(floats.size() == 1);
    CHECK(floats[0] == Approx(0.5));
}

TEST_CASE("Lexer: deeply nested (4 levels)") {
    REQUIRE(lex_ok(R"({"a":{"b":{"c":{"d":1}}}})"));
}

TEST_CASE("Lexer: empty containers") {
    REQUIRE(lex_ok(R"({"a":{},"b":[]})"));
}

TEST_CASE("Lexer: array containing empty object") {
    REQUIRE(lex_ok(R"({"a":[{}]})"));
}

TEST_CASE("Lexer: nested empty arrays") {
    REQUIRE(lex_ok(R"({"a":[[],[]]})"));
}

TEST_CASE("Lexer: whitespace tolerance") {
    REQUIRE(lex_ok("  {  \"a\"  :  1  ,  \"b\"  :  2  }  "));
}

TEST_CASE("Lexer: multiple fields") {
    auto events = lex(R"({"connected":true,"status":"idle {connected}","storage":4,"time":1711500000})");
    auto keys = collect_keys(events);
    REQUIRE(keys.size() == 4);
    CHECK(keys[0] == "connected");
    CHECK(keys[1] == "status");
    CHECK(keys[2] == "storage");
    CHECK(keys[3] == "time");
}

// ═════════════════════════════════════════════════════════════════════════
// JsonLexer: invalid JSON (mirrors sax_parse error tests)
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE("Lexer error: empty input") {
    // No events, but also no error (nothing to parse)
    auto events = lex("");
    bool has_error = false;
    for (auto& ev : events) if (ev.tag == LexerEvent::Error) has_error = true;
    CHECK_FALSE(has_error);
}

TEST_CASE("Lexer error: unexpected character at top level") {
    REQUIRE(lex_error("x"));
}

TEST_CASE("Lexer: bare number at top level is valid JSON") {
    // RFC 8259 allows any value at top level. The lexer accepts this;
    // the SaxParser layer can enforce object-only if needed.
    REQUIRE(lex_ok("123"));
}

TEST_CASE("Lexer error: unterminated string") {
    REQUIRE(lex_error(R"({"a":"hello)"));
}

TEST_CASE("Lexer error: missing colon") {
    REQUIRE(lex_error(R"({"a" 1})"));
}

TEST_CASE("Lexer error: missing comma") {
    REQUIRE(lex_error(R"({"a":1 "b":2})"));
}

TEST_CASE("Lexer error: truncated literal (tru)") {
    REQUIRE(lex_error(R"({"a":tru})"));
}

TEST_CASE("Lexer error: invalid literal") {
    REQUIRE(lex_error(R"({"a":trux})"));
}

TEST_CASE("Lexer error: bare minus") {
    REQUIRE(lex_error(R"({"a":-})"));
}

TEST_CASE("Lexer error: leading zero followed by digit") {
    REQUIRE(lex_error(R"({"a":01})"));
}

TEST_CASE("Lexer error: dot without following digit") {
    REQUIRE(lex_error(R"({"a":1.})"));
}

TEST_CASE("Lexer error: exponent without digit") {
    REQUIRE(lex_error(R"({"a":1e})"));
}

TEST_CASE("Lexer error: exponent sign without digit") {
    REQUIRE(lex_error(R"({"a":1e+})"));
}

TEST_CASE("Lexer error: mismatched brace ([ closed with })") {
    REQUIRE(lex_error("[}"));
}

TEST_CASE("Lexer error: mismatched brace ({ closed with ])") {
    REQUIRE(lex_error("{]"));
}

TEST_CASE("Lexer error: unexpected character in object") {
    REQUIRE(lex_error("{123}"));
}

TEST_CASE("Lexer error: unescaped control character in string") {
    std::string json = R"({"a":"hello)";
    json += '\x01';  // unescaped control char
    json += "\"}";
    REQUIRE(lex_error(json.c_str()));
}

TEST_CASE("Lexer error: invalid escape in string") {
    REQUIRE(lex_error(R"({"a":"\x"})"));
}

TEST_CASE("Lexer error: invalid hex in unicode escape") {
    REQUIRE(lex_error(R"({"a":"\u00GG"})"));
}

TEST_CASE("Lexer error: unterminated escape") {
    // String ending with backslash
    REQUIRE(lex_error("{\"a\":\"\\"));
}

// ═════════════════════════════════════════════════════════════════════════
// JsonLexer: number edge cases via full JSON
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE("Lexer: negative zero") {
    auto events = lex(R"({"a":-0})");
    auto ints = collect_values<int32_t>(events, LexerEvent::Integer);
    REQUIRE(ints.size() == 1);
    CHECK(ints[0] == 0);
}

TEST_CASE("Lexer: negative float") {
    auto events = lex(R"({"a":-3.14})");
    auto floats = collect_values<double>(events, LexerEvent::Float);
    REQUIRE(floats.size() == 1);
    CHECK(floats[0] == Approx(-3.14));
}

TEST_CASE("Lexer: number in array") {
    auto events = lex("[1,-2,3.5]");
    auto ints = collect_values<int32_t>(events, LexerEvent::Integer);
    auto floats = collect_values<double>(events, LexerEvent::Float);
    CHECK(ints.size() == 2);
    CHECK(ints[0] == 1); CHECK(ints[1] == -2);
    CHECK(floats.size() == 1);
    CHECK(floats[0] == Approx(3.5));
}

TEST_CASE("Lexer: number at end of object (terminates on })") {
    auto events = lex(R"({"n":42})");
    REQUIRE(lex_ok(R"({"n":42})"));
    auto ints = collect_values<int32_t>(events, LexerEvent::Integer);
    CHECK(ints.size() == 1);
    CHECK(ints[0] == 42);
}

TEST_CASE("Lexer: number followed by comma") {
    REQUIRE(lex_ok(R"({"a":1,"b":2})"));
}

TEST_CASE("Lexer: negative zero point five") {
    auto events = lex(R"({"a":-0.5})");
    auto floats = collect_values<double>(events, LexerEvent::Float);
    REQUIRE(floats.size() == 1);
    CHECK(floats[0] == Approx(-0.5));
}

TEST_CASE("Lexer: exponent with positive sign") {
    auto events = lex(R"({"a":1e+2})");
    auto floats = collect_values<double>(events, LexerEvent::Float);
    CHECK(floats[0] == Approx(100.0));
}

TEST_CASE("Lexer: exponent with negative sign") {
    auto events = lex(R"({"a":1e-2})");
    auto floats = collect_values<double>(events, LexerEvent::Float);
    CHECK(floats[0] == Approx(0.01));
}

TEST_CASE("Lexer: uppercase E in exponent") {
    auto events = lex(R"({"a":1E3})");
    auto floats = collect_values<double>(events, LexerEvent::Float);
    CHECK(floats[0] == Approx(1000.0));
}

TEST_CASE("Lexer: fraction and exponent combined") {
    auto events = lex(R"({"a":1.5e2})");
    auto floats = collect_values<double>(events, LexerEvent::Float);
    CHECK(floats[0] == Approx(150.0));
}

TEST_CASE("Lexer: large integer 1711500000 (unix timestamp)") {
    auto events = lex(R"({"t":1711500000})");
    auto ints = collect_values<int32_t>(events, LexerEvent::Integer);
    REQUIRE(ints.size() == 1);
    CHECK(ints[0] == 1711500000);
}

// ═════════════════════════════════════════════════════════════════════════
// Equivalence: lexer pipeline vs old SAX parser
//
// Both parse the same JSON; the sink records events. Results must match
// for strings, bools, object structure. Numbers differ in representation
// (old: raw string, new: typed int/float) so we compare parsed values.
// ═════════════════════════════════════════════════════════════════════════

namespace {

struct SinkEvent {
    enum Type { Null, Bool, Int, Float, NumRaw, String, ObjBegin, ObjEnd, ArrBegin, ArrEnd };
    Type type;
    std::string key;
    std::string str_val;
    int32_t int_val = 0;
    double float_val = 0.0;
    bool bool_val = false;
};

/// Recording sink that captures all events in a comparable format.
/// Accepts both on_number (old parser) and on_int/on_float (new).
struct RecSink : note::JsonSink {
    std::vector<SinkEvent> events;

    void on_null(note::string_view k) override {
        events.push_back({SinkEvent::Null, std::string(k), {}, 0, 0, false});
    }
    void on_bool(note::string_view k, bool v) override {
        events.push_back({SinkEvent::Bool, std::string(k), {}, 0, 0, v});
    }
    void on_number(note::string_view k, note::string_view raw) override {
        // Old parser path — parse the raw string to get typed values
        SinkEvent ev{SinkEvent::NumRaw, std::string(k), std::string(raw), 0, 0, false};
        // Try parsing as int first, then float
        bool has_dot = raw.find('.') != note::string_view::npos;
        bool has_exp = raw.find('e') != note::string_view::npos || raw.find('E') != note::string_view::npos;
        if (!has_dot && !has_exp) {
            ev.int_val = note::parse_int(raw);
        } else {
            ev.float_val = note::parse_double(raw);
        }
        events.push_back(ev);
    }
    void on_int(note::string_view k, int32_t v) override {
        events.push_back({SinkEvent::Int, std::string(k), {}, v, 0, false});
    }
    void on_float(note::string_view k, double v) override {
        events.push_back({SinkEvent::Float, std::string(k), {}, 0, v, false});
    }
    void on_string(note::string_view k, note::string_view v) override {
        events.push_back({SinkEvent::String, std::string(k), std::string(v), 0, 0, false});
    }
    void on_object_begin(note::string_view k) override {
        events.push_back({SinkEvent::ObjBegin, std::string(k), {}, 0, 0, false});
    }
    void on_object_end(note::string_view k) override {
        events.push_back({SinkEvent::ObjEnd, std::string(k), {}, 0, 0, false});
    }
    void on_array_begin(note::string_view k) override {
        events.push_back({SinkEvent::ArrBegin, std::string(k), {}, 0, 0, false});
    }
    void on_array_end(note::string_view k) override {
        events.push_back({SinkEvent::ArrEnd, std::string(k), {}, 0, 0, false});
    }
    void reset() override { events.clear(); }
};

void check_equivalence(const char* json) {
    CAPTURE(json);

    // Old parser
    RecSink old_sink;
    auto old_err = note::sax_parse(json, strlen(json), old_sink);
    REQUIRE(old_err.empty());

    // New lexer pipeline
    RecSink new_sink;
    auto new_err = note::sax_lex(json, strlen(json), new_sink);
    REQUIRE(new_err.empty());

    REQUIRE(old_sink.events.size() == new_sink.events.size());
    for (size_t i = 0; i < old_sink.events.size(); ++i) {
        CAPTURE(i);
        auto& o = old_sink.events[i];
        auto& n = new_sink.events[i];

        // Keys must match
        CHECK(o.key == n.key);

        // Event types: NumRaw (old) matches Int or Float (new)
        if (o.type == SinkEvent::NumRaw) {
            if (n.type == SinkEvent::Int) {
                CHECK(o.int_val == n.int_val);
            } else if (n.type == SinkEvent::Float) {
                CHECK(o.float_val == Approx(n.float_val));
            } else {
                FAIL("old parser emitted NumRaw but new didn't emit Int or Float");
            }
        } else {
            // Non-number events must match type exactly
            CHECK(o.type == n.type);
            CHECK(o.str_val == n.str_val);
            CHECK(o.bool_val == n.bool_val);
        }
    }
}

} // namespace

TEST_CASE("Equivalence: simple object") {
    check_equivalence(R"({"status":"connected","rssi":-42})");
}

TEST_CASE("Equivalence: booleans and null") {
    check_equivalence(R"({"ok":true,"n":null,"f":false})");
}

TEST_CASE("Equivalence: nested object") {
    check_equivalence(R"({"inner":{"a":1},"b":"x"})");
}

TEST_CASE("Equivalence: array") {
    check_equivalence(R"({"items":[1,2,3]})");
}

TEST_CASE("Equivalence: escaped strings") {
    check_equivalence(R"({"msg":"hello\nworld","path":"a\\b"})");
}

#if NOTE_UNICODE_ESCAPES
TEST_CASE("Equivalence: unicode escape") {
    check_equivalence(R"({"c":"\u0041"})");
}
#endif

TEST_CASE("Equivalence: number formats") {
    check_equivalence(R"({"a":0,"b":3.14,"c":-1,"d":2.5e10})");
}

TEST_CASE("Equivalence: empty containers") {
    check_equivalence(R"({"a":{},"b":[]})");
}

TEST_CASE("Equivalence: typical notecard response") {
    check_equivalence(R"({"connected":true,"status":"idle {connected}","storage":4,"time":1711500000})");
}

TEST_CASE("Equivalence: deeply nested") {
    check_equivalence(R"({"a":{"b":{"c":{"d":1}}}})");
}

TEST_CASE("Equivalence: array of mixed types") {
    check_equivalence(R"({"a":[1,"two",true,null,3.14]})");
}

// ── CompactNumber unit tests ────────────────────────────────────────────────

TEST_CASE("CompactNumber: positive integer") {
    CompactNumber n;
    n.add_digit(1); n.add_digit(2); n.add_digit(3);
    REQUIRE(n.is_integer());
    REQUIRE(n.to_int32() == 123);
}

TEST_CASE("CompactNumber: negative integer") {
    CompactNumber n;
    n.set_negative();
    n.add_digit(4); n.add_digit(2);
    REQUIRE(n.to_int32() == -42);
}

TEST_CASE("CompactNumber: zero") {
    CompactNumber n;
    n.add_digit(0);
    REQUIRE(n.to_int32() == 0);
}

TEST_CASE("CompactNumber: simple float") {
    CompactNumber n;
    n.add_digit(3);
    n.start_fraction();
    n.add_frac_digit(1); n.add_frac_digit(4);
    REQUIRE_FALSE(n.is_integer());
    REQUIRE(n.to_float() == Approx(3.14).epsilon(0.01));
}

TEST_CASE("CompactNumber: negative float") {
    CompactNumber n;
    n.set_negative();
    n.add_digit(2);
    n.start_fraction();
    n.add_frac_digit(5);
    REQUIRE(n.to_float() == Approx(-2.5).epsilon(0.01));
}

TEST_CASE("CompactNumber: float with exponent") {
    CompactNumber n;
    n.add_digit(1);
    n.start_fraction();
    n.add_frac_digit(5);
    n.start_exponent();
    n.add_exp_digit(2);
    REQUIRE(n.to_float() == Approx(150.0).epsilon(1.0));
}

TEST_CASE("CompactNumber: float with negative exponent") {
    CompactNumber n;
    n.add_digit(5);
    n.start_exponent();
    n.set_exp_negative();
    n.add_exp_digit(1);
    REQUIRE(n.to_float() == Approx(0.5).epsilon(0.01));
}

TEST_CASE("CompactNumber: reset") {
    CompactNumber n;
    n.add_digit(9); n.set_negative();
    n.reset();
    n.add_digit(1);
    REQUIRE(n.to_int32() == 1);
}

TEST_CASE("CompactNumber: large integer within int32 range") {
    CompactNumber n;
    // 2,000,000,000
    n.add_digit(2);
    for (int i = 0; i < 9; ++i) n.add_digit(0);
    REQUIRE(n.to_int32() == 2000000000);
}

TEST_CASE("CompactNumber: fractional precision 6 digits") {
    CompactNumber n;
    n.add_digit(0);
    n.start_fraction();
    n.add_frac_digit(1); n.add_frac_digit(2); n.add_frac_digit(3);
    n.add_frac_digit(4); n.add_frac_digit(5); n.add_frac_digit(6);
    REQUIRE(n.to_float() == Approx(0.123456).epsilon(0.0001));
}

// ── CompactNumber through lexer (integration) ──────────────────────────────

using CompactLexer = JsonLexer<BitStack<uint8_t>, CompactNumber, BasicEscapeDecoder>;

static std::vector<LexerEvent> lex_compact(const char* json) {
    std::vector<LexerEvent> events;
    CompactLexer lexer;
    for (const char* p = json; *p; ++p) {
        lexer.feed(static_cast<uint8_t>(*p), [&](LexerEvent ev) {
            events.push_back(ev);
        });
        if (lexer.has_error()) break;
    }
    return events;
}

TEST_CASE("CompactLexer: integer value") {
    auto events = lex_compact(R"({"value":42})");
    bool found_int = false;
    for (auto& ev : events) {
        if (ev.tag == LexerEvent::Integer) {
            REQUIRE(ev.integer == 42);
            found_int = true;
        }
    }
    REQUIRE(found_int);
}

TEST_CASE("CompactLexer: negative integer") {
    auto events = lex_compact(R"({"v":-99})");
    for (auto& ev : events) {
        if (ev.tag == LexerEvent::Integer) {
            REQUIRE(ev.integer == -99);
            return;
        }
    }
    FAIL("No integer event");
}

TEST_CASE("CompactLexer: float value") {
    auto events = lex_compact(R"({"temp":22.5})");
    for (auto& ev : events) {
        if (ev.tag == LexerEvent::Float) {
            REQUIRE(ev.floating == Approx(22.5).epsilon(0.01));
            return;
        }
    }
    FAIL("No float event");
}

TEST_CASE("CompactLexer: scientific notation") {
    auto events = lex_compact(R"({"big":1.5e3})");
    for (auto& ev : events) {
        if (ev.tag == LexerEvent::Float) {
            REQUIRE(ev.floating == Approx(1500.0).epsilon(1.0));
            return;
        }
    }
    FAIL("No float event");
}

TEST_CASE("CompactLexer: 8-level nesting with BitStack<uint8_t>") {
    // BitStack<uint8_t> supports 8 levels — verify we can nest that deep
    auto events = lex_compact(R"({"a":{"b":{"c":{"d":{"e":{"f":{"g":{"h":1}}}}}}}})");
    bool found_int = false;
    for (auto& ev : events) {
        if (ev.tag == LexerEvent::Integer) {
            REQUIRE(ev.integer == 1);
            found_int = true;
        }
    }
    REQUIRE(found_int);
}

// ═══════════════════════════════════════════════════════════════════════
// Branch coverage — number parsing states
// ═══════════════════════════════════════════════════════════════════════

TEST_CASE("lexer: number with exponent (1e2)") {
    auto events = lex(R"({"v":1e2})");
    bool found = false;
    for (auto& ev : events)
        if (ev.tag == LexerEvent::Float) { CHECK(ev.floating == Approx(100.0)); found = true; }
    CHECK(found);
}

TEST_CASE("lexer: number with positive exponent (1e+2)") {
    auto events = lex(R"({"v":1e+2})");
    bool found = false;
    for (auto& ev : events)
        if (ev.tag == LexerEvent::Float) { CHECK(ev.floating == Approx(100.0)); found = true; }
    CHECK(found);
}

TEST_CASE("lexer: number with negative exponent (1e-2)") {
    auto events = lex(R"({"v":1e-2})");
    bool found = false;
    for (auto& ev : events)
        if (ev.tag == LexerEvent::Float) { CHECK(ev.floating == Approx(0.01)); found = true; }
    CHECK(found);
}

TEST_CASE("lexer: zero with exponent (0e1)") {
    CHECK(lex_ok(R"({"v":0e1})"));
}

TEST_CASE("lexer: zero with fraction (0.5)") {
    auto events = lex(R"({"v":0.5})");
    bool found = false;
    for (auto& ev : events)
        if (ev.tag == LexerEvent::Float) { CHECK(ev.floating == Approx(0.5)); found = true; }
    CHECK(found);
}

TEST_CASE("lexer: fraction then exponent (1.5e2)") {
    auto events = lex(R"({"v":1.5e2})");
    bool found = false;
    for (auto& ev : events)
        if (ev.tag == LexerEvent::Float) { CHECK(ev.floating == Approx(150.0)); found = true; }
    CHECK(found);
}

TEST_CASE("lexer: fraction with negative exponent (1.5e-1)") {
    auto events = lex(R"({"v":1.5e-1})");
    bool found = false;
    for (auto& ev : events)
        if (ev.tag == LexerEvent::Float) { CHECK(ev.floating == Approx(0.15)); found = true; }
    CHECK(found);
}

TEST_CASE("lexer: error — dot without following digit") {
    CHECK(lex_error(R"({"v":1.})"));
}

TEST_CASE("lexer: error — exponent without digit") {
    CHECK(lex_error(R"({"v":1e})"));
}

TEST_CASE("lexer: error — minus without digit") {
    CHECK(lex_error(R"({"v":-})"));
}

TEST_CASE("lexer: error — exponent sign without digit") {
    CHECK(lex_error(R"({"v":1e+})"));
}

// ═══════════════════════════════════════════════════════════════════════
// Branch coverage — escape sequences
// ═══════════════════════════════════════════════════════════════════════

TEST_CASE("lexer: escape tab") {
    auto events = lex(R"({"v":"a\tb"})");
    bool found = false;
    for (auto& ev : events)
        if (ev.tag == LexerEvent::StringChar && ev.ch == '\t') found = true;
    CHECK(found);
}

TEST_CASE("lexer: escape carriage return") {
    auto events = lex(R"({"v":"a\rb"})");
    bool found = false;
    for (auto& ev : events)
        if (ev.tag == LexerEvent::StringChar && ev.ch == '\r') found = true;
    CHECK(found);
}

TEST_CASE("lexer: escape backspace and formfeed") {
    auto events = lex(R"({"v":"\b\f"})");
    bool found_b = false, found_f = false;
    for (auto& ev : events) {
        if (ev.tag == LexerEvent::StringChar && ev.ch == '\b') found_b = true;
        if (ev.tag == LexerEvent::StringChar && ev.ch == '\f') found_f = true;
    }
    CHECK(found_b);
    CHECK(found_f);
}

TEST_CASE("lexer: escape backslash and forward slash") {
    auto events = lex(R"({"v":"\\\/"})");
    bool found_back = false, found_fwd = false;
    for (auto& ev : events) {
        if (ev.tag == LexerEvent::StringChar && ev.ch == '\\') found_back = true;
        if (ev.tag == LexerEvent::StringChar && ev.ch == '/') found_fwd = true;
    }
    CHECK(found_back);
    CHECK(found_fwd);
}

#if NOTE_UNICODE_ESCAPES  // Unicode escapes require Utf8EscapeDecoder (disabled when NOTE_UNICODE_ESCAPES=0)
TEST_CASE("lexer: unicode escape \\u0041 = A") {
    auto events = lex(R"({"v":"\u0041"})");
    bool found = false;
    for (auto& ev : events)
        if (ev.tag == LexerEvent::StringChar && ev.ch == 'A') found = true;
    CHECK(found);
}

TEST_CASE("lexer: unicode escape in key") {
    auto events = lex(R"({"\u006B":1})");
    bool found = false;
    for (auto& ev : events)
        if (ev.tag == LexerEvent::KeyChar && ev.ch == 'k') found = true;
    CHECK(found);
}

TEST_CASE("lexer: invalid unicode escape") {
    CHECK(lex_error(R"({"v":"\u00GG"})"));
}
#endif

// ═══════════════════════════════════════════════════════════════════════
// Branch coverage — whitespace variants
// ═══════════════════════════════════════════════════════════════════════

TEST_CASE("lexer: tab and CR whitespace in object") {
    CHECK(lex_ok("{\t\"a\"\r:\t1\r}"));
}

// ═══════════════════════════════════════════════════════════════════════
// Branch coverage — error state and stack overflow
// ═══════════════════════════════════════════════════════════════════════

TEST_CASE("lexer: bytes after error are ignored") {
    DefaultLexer lexer;
    std::vector<LexerEvent> events;
    // Feed invalid JSON
    const char* bad = "{@";
    for (const char* p = bad; *p; ++p)
        lexer.feed(static_cast<uint8_t>(*p), [&](LexerEvent ev) { events.push_back(ev); });
    CHECK(lexer.has_error());
    size_t count_before = events.size();
    // Feed more bytes — should be ignored
    lexer.feed('x', [&](LexerEvent ev) { events.push_back(ev); });
    CHECK(events.size() == count_before);
}

TEST_CASE("lexer: stack overflow on deep nesting") {
    // DefaultLexer uses BitStack<uint16_t> = 16 levels. Exceed that.
    std::string json(17, '{');
    // This should produce an error when the stack is full
    auto events = lex(json.c_str());
    bool has_error = false;
    for (auto& ev : events)
        if (ev.tag == LexerEvent::Error) has_error = true;
    CHECK(has_error);
}

TEST_CASE("CompactLexer: stack overflow on deep nesting") {
    // CompactLexer uses BitStack<uint8_t> = 8 levels.
    std::string json(9, '{');
    auto events = lex_compact(json.c_str());
    bool has_error = false;
    for (auto& ev : events)
        if (ev.tag == LexerEvent::Error) has_error = true;
    CHECK(has_error);
}
