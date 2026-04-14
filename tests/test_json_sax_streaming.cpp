// Tests for StreamingSaxParser — incremental SAX parsing from a byte source.
//
// Uses a ByteFeeder that delivers bytes in configurable chunk sizes,
// from a single byte at a time up to the full response.

#include "catch.hpp"

#include <note/json_sax_streaming.hpp>

#include <cstring>
#include <string>
#include <vector>

using namespace note;

// ---------------------------------------------------------------------------
// ByteFeeder — simulates a transport that delivers bytes in chunks
// ---------------------------------------------------------------------------

struct ByteFeeder {
    const char* data;
    size_t len;
    size_t pos = 0;
    size_t chunk_size;  // max bytes per read

    ByteFeeder(const char* d, size_t l, size_t chunk)
        : data(d), len(l), chunk_size(chunk) {}

    Result<size_t> read(uint8_t* buf, size_t max, uint32_t /*timeout_ms*/) {
        if (pos >= len) return size_t(0);
        size_t n = std::min({max, chunk_size, len - pos});
        memcpy(buf, data + pos, n);
        pos += n;
        return n;
    }
};

// ---------------------------------------------------------------------------
// RecordingSink — captures all SAX events for verification
// ---------------------------------------------------------------------------

struct Event {
    enum Type { Null, Bool, Number, String, ObjBegin, ObjEnd, ArrBegin, ArrEnd };
    Type type;
    std::string key;
    std::string value;
};

struct RecordingSink : public JsonSink {
    std::vector<Event> events;

    void on_null(string_view key) override {
        events.push_back({Event::Null, std::string(key.data(), key.size()), "null"});
    }
    void on_bool(string_view key, bool v) override {
        events.push_back({Event::Bool, std::string(key.data(), key.size()), v ? "true" : "false"});
    }
    void on_number(string_view key, string_view raw) override {
        events.push_back({Event::Number, std::string(key.data(), key.size()),
                          std::string(raw.data(), raw.size())});
    }
    void on_string(string_view key, string_view val) override {
        events.push_back({Event::String, std::string(key.data(), key.size()),
                          std::string(val.data(), val.size())});
    }
    void on_object_begin(string_view key) override {
        events.push_back({Event::ObjBegin, std::string(key.data(), key.size()), "{"});
    }
    void on_object_end(string_view key) override {
        events.push_back({Event::ObjEnd, std::string(key.data(), key.size()), "}"});
    }
    void on_array_begin(string_view key) override {
        events.push_back({Event::ArrBegin, std::string(key.data(), key.size()), "["});
    }
    void on_array_end(string_view key) override {
        events.push_back({Event::ArrEnd, std::string(key.data(), key.size()), "]"});
    }
};

// ---------------------------------------------------------------------------
// Helper: parse at a given chunk size and verify events
// ---------------------------------------------------------------------------

static std::vector<Event> parse_with_chunks(const char* json, size_t chunk_size) {
    ByteFeeder feeder(json, strlen(json), chunk_size);
    RecordingSink sink;

    auto read_fn = [&](uint8_t* buf, size_t max, uint32_t timeout) -> Result<size_t> {
        return feeder.read(buf, max, timeout);
    };

    auto err = sax_parse_streaming(read_fn, 5000, sink);
    REQUIRE(err.empty());
    return sink.events;
}


// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_CASE("streaming sax: simple object, full buffer") {
    auto events = parse_with_chunks(R"({"status":"connected","rssi":-42})", 256);

    REQUIRE(events.size() == 4);  // { + 2 fields + }
    CHECK(events[0].type == Event::ObjBegin);
    CHECK(events[1].type == Event::String);
    CHECK(events[1].key == "status");
    CHECK(events[1].value == "connected");
    CHECK(events[2].type == Event::Number);
    CHECK(events[2].key == "rssi");
    CHECK(events[2].value == "-42");
    CHECK(events[3].type == Event::ObjEnd);
}

TEST_CASE("streaming sax: simple object, one byte at a time") {
    auto events = parse_with_chunks(R"({"status":"connected","rssi":-42})", 1);

    REQUIRE(events.size() == 4);
    CHECK(events[1].key == "status");
    CHECK(events[1].value == "connected");
    CHECK(events[2].key == "rssi");
    CHECK(events[2].value == "-42");
}

TEST_CASE("streaming sax: 3-byte chunks") {
    auto events = parse_with_chunks(R"({"ok":true,"n":null,"f":false})", 3);

    REQUIRE(events.size() == 5);  // { + 3 fields + }
    CHECK(events[1].type == Event::Bool);
    CHECK(events[1].key == "ok");
    CHECK(events[1].value == "true");
    CHECK(events[2].type == Event::Null);
    CHECK(events[2].key == "n");
    CHECK(events[3].type == Event::Bool);
    CHECK(events[3].key == "f");
    CHECK(events[3].value == "false");
}

TEST_CASE("streaming sax: nested object") {
    const char* json = R"({"inner":{"a":1},"b":"x"})";
    auto events = parse_with_chunks(json, 4);

    // {, inner:{, a:1, }, b:"x", }
    CHECK(events[0].type == Event::ObjBegin);  // root
    CHECK(events[1].type == Event::ObjBegin);  // inner
    CHECK(events[1].key == "inner");
    CHECK(events[2].type == Event::Number);
    CHECK(events[2].key == "a");
    CHECK(events[2].value == "1");
    CHECK(events[3].type == Event::ObjEnd);    // inner
    CHECK(events[4].type == Event::String);
    CHECK(events[4].key == "b");
    CHECK(events[4].value == "x");
    CHECK(events[5].type == Event::ObjEnd);    // root
}

TEST_CASE("streaming sax: array") {
    const char* json = R"({"items":[1,2,3]})";
    auto events = parse_with_chunks(json, 2);

    CHECK(events[0].type == Event::ObjBegin);
    CHECK(events[1].type == Event::ArrBegin);
    CHECK(events[1].key == "items");
    CHECK(events[2].type == Event::Number);
    CHECK(events[2].value == "1");
    CHECK(events[3].type == Event::Number);
    CHECK(events[3].value == "2");
    CHECK(events[4].type == Event::Number);
    CHECK(events[4].value == "3");
    CHECK(events[5].type == Event::ArrEnd);
    CHECK(events[6].type == Event::ObjEnd);
}

TEST_CASE("streaming sax: escaped strings one byte at a time") {
    const char* json = R"({"msg":"hello\nworld","path":"a\\b"})";
    auto events = parse_with_chunks(json, 1);

    REQUIRE(events.size() == 4);  // { + 2 fields + }
    CHECK(events[1].key == "msg");
    CHECK(events[1].value == "hello\nworld");
    CHECK(events[2].key == "path");
    CHECK(events[2].value == "a\\b");
}

TEST_CASE("streaming sax: unicode escape") {
    const char* json = R"({"c":"\u0041"})";
    auto events = parse_with_chunks(json, 1);

    CHECK(events[1].value == "A");
}

TEST_CASE("streaming sax: number formats") {
    const char* json = R"({"a":0,"b":3.14,"c":-1,"d":2.5e10})";
    auto events = parse_with_chunks(json, 5);

    CHECK(events[1].value == "0");
    CHECK(events[2].value == "3.14");
    CHECK(events[3].value == "-1");
    CHECK(events[4].value == "2.5e10");
}

TEST_CASE("streaming sax: empty object") {
    auto events = parse_with_chunks("{}", 1);
    REQUIRE(events.size() == 2);
    CHECK(events[0].type == Event::ObjBegin);
    CHECK(events[1].type == Event::ObjEnd);
}

TEST_CASE("streaming sax: read error returns parse error") {
    auto read_fn = [](uint8_t*, size_t, uint32_t) -> Result<size_t> {
        return make_error(Error::ResponseLost, Cause::Timeout, "timeout");
    };

    RecordingSink sink;
    auto err = sax_parse_streaming(read_fn, 5000, sink);
    REQUIRE(!err.empty());
}

TEST_CASE("streaming sax: matches buffer sax parser output") {
    // Verify streaming parser produces identical events to buffer parser
    const char* json = R"({"connected":true,"status":"idle","rssi":-55,"temp":23.7})";

    // Buffer parser
    RecordingSink buf_sink;
    auto buf_err = sax_parse(json, strlen(json), buf_sink);
    REQUIRE(buf_err.empty());

    // Streaming parser — one byte at a time (worst case)
    auto stream_events = parse_with_chunks(json, 1);

    REQUIRE(stream_events.size() == buf_sink.events.size());
    for (size_t i = 0; i < stream_events.size(); ++i) {
        CHECK(stream_events[i].type == buf_sink.events[i].type);
        CHECK(stream_events[i].key == buf_sink.events[i].key);
        CHECK(stream_events[i].value == buf_sink.events[i].value);
    }
}

TEST_CASE("streaming sax: typical notecard response") {
    const char* json =
        R"({"connected":true,"status":"idle {connected}","storage":4,"time":1711500000})";

    // Test at every chunk size from 1 to full buffer
    size_t json_len = strlen(json);
    for (size_t chunk = 1; chunk <= json_len; ++chunk) {
        auto events = parse_with_chunks(json, chunk);
        REQUIRE(events.size() == 6);  // { + 4 fields + }
        CHECK(events[1].key == "connected");
        CHECK(events[1].value == "true");
        CHECK(events[2].key == "status");
        CHECK(events[2].value == "idle {connected}");
        CHECK(events[3].key == "storage");
        CHECK(events[3].value == "4");
        CHECK(events[4].key == "time");
        CHECK(events[4].value == "1711500000");
    }
}

// ---------------------------------------------------------------------------
// Edge case tests
// ---------------------------------------------------------------------------

// ── 1. Long strings exceeding scratch ────────────────────────────────────

TEST_CASE("streaming sax: long value string is silently truncated") {
    // Default value scratch is 256 bytes. Build a 300+ char value.
    std::string long_val(320, 'x');
    std::string json = R"({"k":")" + long_val + R"("})";

    // Should not crash at any chunk size
    for (size_t chunk : {size_t(1), size_t(7), size_t(64), size_t(512)}) {
        auto events = parse_with_chunks(json.c_str(), chunk);
        REQUIRE(events.size() == 3);
        CHECK(events[1].key == "k");
        // Value should be truncated to 256 (default val scratch size)
        CHECK(events[1].value.size() == 256);
        CHECK(events[1].value == std::string(256, 'x'));
    }
}

TEST_CASE("streaming sax: long key string is silently truncated") {
    // Default key scratch is 64 bytes. Build an 80+ char key.
    std::string long_key(80, 'k');
    std::string json = R"({")" + long_key + R"(":1})";

    for (size_t chunk : {size_t(1), size_t(5), size_t(256)}) {
        auto events = parse_with_chunks(json.c_str(), chunk);
        REQUIRE(events.size() == 3);
        // Key should be truncated to 64 (default key scratch size)
        CHECK(events[1].key.size() == 64);
        CHECK(events[1].key == std::string(64, 'k'));
        CHECK(events[1].value == "1");
    }
}

// ── 2. Deeply nested objects ─────────────────────────────────────────────

TEST_CASE("streaming sax: deeply nested objects (4 levels)") {
    const char* json = R"({"a":{"b":{"c":{"d":1}}}})";

    for (size_t chunk : {size_t(1), size_t(3)}) {
        auto events = parse_with_chunks(json, chunk);
        // { a:{ b:{ c:{ d:1 } } } }
        REQUIRE(events.size() == 9);
        CHECK(events[0].type == Event::ObjBegin);
        CHECK(events[1].type == Event::ObjBegin);
        CHECK(events[1].key == "a");
        CHECK(events[2].type == Event::ObjBegin);
        CHECK(events[2].key == "b");
        CHECK(events[3].type == Event::ObjBegin);
        CHECK(events[3].key == "c");
        CHECK(events[4].type == Event::Number);
        CHECK(events[4].key == "d");
        CHECK(events[4].value == "1");
        CHECK(events[5].type == Event::ObjEnd);
        CHECK(events[6].type == Event::ObjEnd);
        CHECK(events[7].type == Event::ObjEnd);
        CHECK(events[8].type == Event::ObjEnd);
    }
}

// ── 3. Empty containers in various positions ─────────────────────────────

TEST_CASE("streaming sax: empty object and empty array") {
    const char* json = R"({"a":{},"b":[]})";
    for (size_t chunk : {size_t(1), size_t(3), size_t(64)}) {
        auto events = parse_with_chunks(json, chunk);
        // { a:{ } b:[ ] }
        REQUIRE(events.size() == 6);
        CHECK(events[0].type == Event::ObjBegin);
        CHECK(events[1].type == Event::ObjBegin);
        CHECK(events[1].key == "a");
        CHECK(events[2].type == Event::ObjEnd);
        CHECK(events[3].type == Event::ArrBegin);
        CHECK(events[3].key == "b");
        CHECK(events[4].type == Event::ArrEnd);
        CHECK(events[5].type == Event::ObjEnd);
    }
}

TEST_CASE("streaming sax: array containing empty object") {
    const char* json = R"({"a":[{}]})";
    for (size_t chunk : {size_t(1), size_t(2), size_t(64)}) {
        auto events = parse_with_chunks(json, chunk);
        // { a:[ { } ] }
        REQUIRE(events.size() == 6);
        CHECK(events[0].type == Event::ObjBegin);
        CHECK(events[1].type == Event::ArrBegin);
        CHECK(events[1].key == "a");
        CHECK(events[2].type == Event::ObjBegin);
        CHECK(events[3].type == Event::ObjEnd);
        CHECK(events[4].type == Event::ArrEnd);
        CHECK(events[5].type == Event::ObjEnd);
    }
}

TEST_CASE("streaming sax: nested empty arrays") {
    const char* json = R"({"a":[[],[]]})";
    for (size_t chunk : {size_t(1), size_t(4), size_t(64)}) {
        auto events = parse_with_chunks(json, chunk);
        // { a:[ [ ] [ ] ] }
        REQUIRE(events.size() == 8);
        CHECK(events[0].type == Event::ObjBegin);
        CHECK(events[1].type == Event::ArrBegin);
        CHECK(events[1].key == "a");
        CHECK(events[2].type == Event::ArrBegin);
        CHECK(events[3].type == Event::ArrEnd);
        CHECK(events[4].type == Event::ArrBegin);
        CHECK(events[5].type == Event::ArrEnd);
        CHECK(events[6].type == Event::ArrEnd);
        CHECK(events[7].type == Event::ObjEnd);
    }
}

// ── 4. Whitespace-heavy JSON ─────────────────────────────────────────────

TEST_CASE("streaming sax: whitespace-heavy JSON with small chunks") {
    const char* json = R"({  "a"  :  1  ,  "b"  :  2  })";
    for (size_t chunk : {size_t(1), size_t(2), size_t(3)}) {
        auto events = parse_with_chunks(json, chunk);
        REQUIRE(events.size() == 4);
        CHECK(events[0].type == Event::ObjBegin);
        CHECK(events[1].type == Event::Number);
        CHECK(events[1].key == "a");
        CHECK(events[1].value == "1");
        CHECK(events[2].type == Event::Number);
        CHECK(events[2].key == "b");
        CHECK(events[2].value == "2");
        CHECK(events[3].type == Event::ObjEnd);
    }
}

// ── 5. Malformed JSON error paths ────────────────────────────────────────

TEST_CASE("streaming sax: error on unterminated string") {
    const char* json = R"({"a":"hello)";
    ByteFeeder feeder(json, strlen(json), 256);
    RecordingSink sink;
    auto read_fn = [&](uint8_t* buf, size_t max, uint32_t timeout) -> Result<size_t> {
        return feeder.read(buf, max, timeout);
    };
    auto err = sax_parse_streaming(read_fn, 5000, sink);
    REQUIRE(!err.empty());
}

TEST_CASE("streaming sax: error on missing colon") {
    const char* json = R"({"a" 1})";
    ByteFeeder feeder(json, strlen(json), 256);
    RecordingSink sink;
    auto read_fn = [&](uint8_t* buf, size_t max, uint32_t timeout) -> Result<size_t> {
        return feeder.read(buf, max, timeout);
    };
    auto err = sax_parse_streaming(read_fn, 5000, sink);
    REQUIRE(!err.empty());
}

TEST_CASE("streaming sax: error on missing comma") {
    const char* json = R"({"a":1 "b":2})";
    ByteFeeder feeder(json, strlen(json), 256);
    RecordingSink sink;
    auto read_fn = [&](uint8_t* buf, size_t max, uint32_t timeout) -> Result<size_t> {
        return feeder.read(buf, max, timeout);
    };
    auto err = sax_parse_streaming(read_fn, 5000, sink);
    REQUIRE(!err.empty());
}

TEST_CASE("streaming sax: error on truncated literal") {
    const char* json = R"({"a":tru})";
    ByteFeeder feeder(json, strlen(json), 256);
    RecordingSink sink;
    auto read_fn = [&](uint8_t* buf, size_t max, uint32_t timeout) -> Result<size_t> {
        return feeder.read(buf, max, timeout);
    };
    auto err = sax_parse_streaming(read_fn, 5000, sink);
    REQUIRE(!err.empty());
}

TEST_CASE("streaming sax: error on bare minus") {
    const char* json = R"({"a":-})";
    ByteFeeder feeder(json, strlen(json), 256);
    RecordingSink sink;
    auto read_fn = [&](uint8_t* buf, size_t max, uint32_t timeout) -> Result<size_t> {
        return feeder.read(buf, max, timeout);
    };
    auto err = sax_parse_streaming(read_fn, 5000, sink);
    REQUIRE(!err.empty());
}

TEST_CASE("streaming sax: error on leading zero in number") {
    // Strict JSON rejects 01 as a number. Our parser consumes '0', then
    // finds '1' where it expects ',' or '}', producing an error.
    const char* json = R"({"a":01})";
    ByteFeeder feeder(json, strlen(json), 256);
    RecordingSink sink;
    auto read_fn = [&](uint8_t* buf, size_t max, uint32_t timeout) -> Result<size_t> {
        return feeder.read(buf, max, timeout);
    };
    auto err = sax_parse_streaming(read_fn, 5000, sink);
    REQUIRE(!err.empty());
}

TEST_CASE("streaming sax: error on empty input") {
    const char* json = "";
    ByteFeeder feeder(json, strlen(json), 256);
    RecordingSink sink;
    auto read_fn = [&](uint8_t* buf, size_t max, uint32_t timeout) -> Result<size_t> {
        return feeder.read(buf, max, timeout);
    };
    auto err = sax_parse_streaming(read_fn, 5000, sink);
    REQUIRE(!err.empty());
}

TEST_CASE("streaming sax: error on just whitespace") {
    const char* json = "   \t\n  ";
    ByteFeeder feeder(json, strlen(json), 256);
    RecordingSink sink;
    auto read_fn = [&](uint8_t* buf, size_t max, uint32_t timeout) -> Result<size_t> {
        return feeder.read(buf, max, timeout);
    };
    auto err = sax_parse_streaming(read_fn, 5000, sink);
    REQUIRE(!err.empty());
}

// ── Branch coverage: number parsing edge cases ──────────────────────────

static string_view parse_error(const char* json, size_t chunk = 256) {
    ByteFeeder feeder(json, strlen(json), chunk);
    RecordingSink sink;
    auto read_fn = [&](uint8_t* buf, size_t max, uint32_t timeout) -> Result<size_t> {
        return feeder.read(buf, max, timeout);
    };
    return sax_parse_streaming(read_fn, 5000, sink);
}

TEST_CASE("streaming sax: trailing decimal error") {
    auto err = parse_error(R"({"a":1.})");
    REQUIRE(!err.empty());
    // Also at 1-byte chunks
    err = parse_error(R"({"a":1.})", 1);
    REQUIRE(!err.empty());
}

TEST_CASE("streaming sax: bare exponent error") {
    auto err = parse_error(R"({"a":1e})");
    REQUIRE(!err.empty());
}

TEST_CASE("streaming sax: exponent with sign only error") {
    auto err = parse_error(R"({"a":1e+})");
    REQUIRE(!err.empty());
}

TEST_CASE("streaming sax: valid plus sign in exponent") {
    auto events = parse_with_chunks(R"({"a":1e+2})", 1);
    REQUIRE(events.size() == 3);
    CHECK(events[1].value == "1e+2");
}

TEST_CASE("streaming sax: valid minus sign in exponent") {
    auto events = parse_with_chunks(R"({"a":1E-3})", 1);
    REQUIRE(events.size() == 3);
    CHECK(events[1].value == "1E-3");
}

TEST_CASE("streaming sax: number exceeding value buffer") {
    // Build a number longer than the default 256-byte val scratch
    std::string big_num(300, '9');
    std::string json = R"({"n":)" + big_num + "}";
    auto events = parse_with_chunks(json.c_str(), 8);
    REQUIRE(events.size() == 3);
    CHECK(events[1].key == "n");
    // Value should be truncated to 256 (default val scratch)
    CHECK(events[1].value.size() == 256);
}

TEST_CASE("streaming sax: number with overflow in decimal digits") {
    // Long decimal part that exceeds value buffer
    std::string json = R"({"n":0.)" + std::string(300, '1') + "}";
    auto events = parse_with_chunks(json.c_str(), 8);
    REQUIRE(events.size() == 3);
    CHECK(events[1].key == "n");
}

TEST_CASE("streaming sax: number with overflow in exponent digits") {
    std::string json = R"({"n":1e)" + std::string(300, '9') + "}";
    auto events = parse_with_chunks(json.c_str(), 8);
    REQUIRE(events.size() == 3);
    CHECK(events[1].key == "n");
}

// ── Branch coverage: escape sequence edge cases ─────────────────────────

TEST_CASE("streaming sax: all escape sequences at 1-byte chunks") {
    // Test \/ \b \f \r (already tested: \" \\ \n \t \u)
    const char* json = R"({"a":"\/\b\f\r"})";
    auto events = parse_with_chunks(json, 1);
    REQUIRE(events.size() == 3);
    CHECK(events[1].value == "/\b\f\r");
}

TEST_CASE("streaming sax: invalid escape character") {
    // \x is not a valid JSON escape
    const char* json = R"({"a":"\x"})";
    // Our parser treats unknown escapes as literal (outputs the char after backslash)
    auto events = parse_with_chunks(json, 1);
    REQUIRE(events.size() == 3);
    CHECK(events[1].value == "x");
}

TEST_CASE("streaming sax: unicode 2-byte UTF-8") {
    // \u00E9 = 'é' = 2-byte UTF-8: 0xC3 0xA9
    const char* json = R"({"c":"\u00E9"})";
    auto events = parse_with_chunks(json, 1);
    REQUIRE(events.size() == 3);
    CHECK(events[1].value == "\xC3\xA9");
}

TEST_CASE("streaming sax: unicode 3-byte UTF-8") {
    // \u4E16 = '世' = 3-byte UTF-8: 0xE4 0xB8 0x96
    const char* json = R"({"c":"\u4E16"})";
    auto events = parse_with_chunks(json, 1);
    REQUIRE(events.size() == 3);
    CHECK(events[1].value == "\xE4\xB8\x96");
}

TEST_CASE("streaming sax: unicode surrogate replaced with ?") {
    // \uD800 is a surrogate — should be replaced with '?'
    const char* json = R"({"c":"\uD800"})";
    auto events = parse_with_chunks(json, 1);
    REQUIRE(events.size() == 3);
    CHECK(events[1].value == "?");
}

TEST_CASE("streaming sax: invalid hex in unicode escape") {
    const char* json = R"({"a":"\uGGGG"})";
    auto err = parse_error(json, 1);
    REQUIRE(!err.empty());
}

TEST_CASE("streaming sax: incomplete unicode escape at EOF") {
    const char* json = R"({"a":"\u00)";
    auto err = parse_error(json, 1);
    REQUIRE(!err.empty());
}

// ── Branch coverage: literal keywords split at 1-byte ───────────────────

TEST_CASE("streaming sax: true/false/null all split at 1-byte boundary") {
    auto events = parse_with_chunks(R"({"a":true,"b":false,"c":null})", 1);
    REQUIRE(events.size() == 5);
    CHECK(events[1].type == Event::Bool);
    CHECK(events[1].value == "true");
    CHECK(events[2].type == Event::Bool);
    CHECK(events[2].value == "false");
    CHECK(events[3].type == Event::Null);
}

// ── Branch coverage: truncated input / EOF ──────────────────────────────

TEST_CASE("streaming sax: EOF after key colon") {
    auto err = parse_error(R"({"a":)");
    REQUIRE(!err.empty());
}

TEST_CASE("streaming sax: EOF in array after comma") {
    auto err = parse_error(R"({"a":[1,)");
    REQUIRE(!err.empty());
}

TEST_CASE("streaming sax: EOF after nested object") {
    auto err = parse_error(R"({"a":{})");
    REQUIRE(!err.empty());
}

TEST_CASE("streaming sax: EOF during number") {
    auto err = parse_error(R"({"a":12)");
    REQUIRE(!err.empty());
}

TEST_CASE("streaming sax: EOF after minus sign") {
    auto err = parse_error(R"({"a":-)");
    REQUIRE(!err.empty());
}

// ── Branch coverage: unexpected character ────────────────────────────────

TEST_CASE("streaming sax: unexpected character at value position") {
    auto err = parse_error(R"({"a":$})");
    REQUIRE(!err.empty());
}

// ── Branch coverage: control character in string ─────────────────────────

TEST_CASE("streaming sax: control character in string") {
    std::string json = R"({"a":")";
    json += '\x01';
    json += R"("})";
    auto err = parse_error(json.c_str(), 1);
    REQUIRE(!err.empty());
}

// ── Branch coverage: escape at EOF ──────────────────────────────────────

TEST_CASE("streaming sax: backslash at end of input") {
    std::string json = R"({"a":"\)";
    auto err = parse_error(json.c_str(), 1);
    REQUIRE(!err.empty());
}

// ── 6. SaxStreamBuf with caller-provided buffers ─────────────────────────

TEST_CASE("streaming sax: explicit SaxStreamBuf with small buffer") {
    const char* json = R"({"status":"ok","val":42})";

    // 96 bytes total: partitioned as 16 read + 16 key + 64 value
    char storage[96];
    SaxStreamBuf sbuf(storage);

    ByteFeeder feeder(json, strlen(json), 256);
    RecordingSink sink;
    auto read_fn = [&](uint8_t* buf, size_t max, uint32_t timeout) -> Result<size_t> {
        return feeder.read(buf, max, timeout);
    };
    auto err = sax_parse_streaming(read_fn, 5000, sbuf, sink);
    REQUIRE(err.empty());
    REQUIRE(sink.events.size() == 4);
    CHECK(sink.events[1].key == "status");
    CHECK(sink.events[1].value == "ok");
    CHECK(sink.events[2].key == "val");
    CHECK(sink.events[2].value == "42");
}

TEST_CASE("streaming sax: explicit 3-region SaxStreamBuf") {
    const char* json = R"({"x":"hello","y":true})";

    // Separate buffers with explicit control
    uint8_t rbuf[16];
    char kbuf[32];
    char vbuf[64];
    SaxStreamBuf sbuf(rbuf, sizeof(rbuf), kbuf, sizeof(kbuf), vbuf, sizeof(vbuf));

    ByteFeeder feeder(json, strlen(json), 256);
    RecordingSink sink;
    auto read_fn = [&](uint8_t* buf, size_t max, uint32_t timeout) -> Result<size_t> {
        return feeder.read(buf, max, timeout);
    };
    auto err = sax_parse_streaming(read_fn, 5000, sbuf, sink);
    REQUIRE(err.empty());
    REQUIRE(sink.events.size() == 4);
    CHECK(sink.events[1].key == "x");
    CHECK(sink.events[1].value == "hello");
    CHECK(sink.events[2].key == "y");
    CHECK(sink.events[2].value == "true");
}

TEST_CASE("streaming sax: very small read buffer triggers frequent refills") {
    const char* json = R"({"name":"test","count":7,"flag":false})";

    // 8-byte read buffer with normal scratch sizes
    uint8_t rbuf[8];
    char kbuf[32];
    char vbuf[128];
    SaxStreamBuf sbuf(rbuf, sizeof(rbuf), kbuf, sizeof(kbuf), vbuf, sizeof(vbuf));

    ByteFeeder feeder(json, strlen(json), 256);
    RecordingSink sink;
    auto read_fn = [&](uint8_t* buf, size_t max, uint32_t timeout) -> Result<size_t> {
        return feeder.read(buf, max, timeout);
    };
    auto err = sax_parse_streaming(read_fn, 5000, sbuf, sink);
    REQUIRE(err.empty());
    REQUIRE(sink.events.size() == 5);
    CHECK(sink.events[1].key == "name");
    CHECK(sink.events[1].value == "test");
    CHECK(sink.events[2].key == "count");
    CHECK(sink.events[2].value == "7");
    CHECK(sink.events[3].key == "flag");
    CHECK(sink.events[3].value == "false");
}

// ── 7. Large number of fields ────────────────────────────────────────────

TEST_CASE("streaming sax: 15+ fields in one object at 5-byte chunks") {
    const char* json =
        R"({"f1":1,"f2":2,"f3":3,"f4":4,"f5":5,"f6":6,"f7":7,"f8":8,)"
        R"("f9":9,"f10":10,"f11":11,"f12":12,"f13":13,"f14":14,"f15":15,"f16":16})";

    auto events = parse_with_chunks(json, 5);

    // { + 16 fields + }
    REQUIRE(events.size() == 18);
    CHECK(events[0].type == Event::ObjBegin);
    for (int i = 1; i <= 16; ++i) {
        CHECK(events[static_cast<size_t>(i)].type == Event::Number);
        CHECK(events[static_cast<size_t>(i)].key == "f" + std::to_string(i));
        CHECK(events[static_cast<size_t>(i)].value == std::to_string(i));
    }
    CHECK(events[17].type == Event::ObjEnd);
}

// ── 8. Multiple consecutive parses ───────────────────────────────────────

TEST_CASE("streaming sax: multiple consecutive parses through same infrastructure") {
    const char* jsons[] = {
        R"({"a":1})",
        R"({"b":"two","c":true})",
        R"({"d":[3,4],"e":null})",
    };

    // Parse 3 different JSON strings, verifying no state leaks between parses
    for (int round = 0; round < 3; ++round) {
        auto events = parse_with_chunks(jsons[round], 3);

        switch (round) {
        case 0:
            REQUIRE(events.size() == 3);
            CHECK(events[1].key == "a");
            CHECK(events[1].value == "1");
            break;
        case 1:
            REQUIRE(events.size() == 4);
            CHECK(events[1].key == "b");
            CHECK(events[1].value == "two");
            CHECK(events[2].key == "c");
            CHECK(events[2].value == "true");
            break;
        case 2:
            REQUIRE(events.size() == 7);
            CHECK(events[1].type == Event::ArrBegin);
            CHECK(events[1].key == "d");
            CHECK(events[2].value == "3");
            CHECK(events[3].value == "4");
            CHECK(events[4].type == Event::ArrEnd);
            CHECK(events[5].type == Event::Null);
            CHECK(events[5].key == "e");
            CHECK(events[6].type == Event::ObjEnd);
            break;
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Branch coverage: additional tests targeting uncovered branches
// ═══════════════════════════════════════════════════════════════════════════

// --- Helper that returns error string for a given JSON ---
static string_view parse_streaming_error(const char* json, size_t chunk = 256) {
    ByteFeeder feeder(json, strlen(json), chunk);
    RecordingSink sink;
    auto read_fn = [&](uint8_t* buf, size_t max, uint32_t timeout) -> Result<size_t> {
        return feeder.read(buf, max, timeout);
    };
    return sax_parse_streaming(read_fn, 5000, sink);
}

// --- Helper that returns events, asserting success ---
static std::vector<Event> parse_streaming_ok(const char* json, size_t chunk = 256) {
    ByteFeeder feeder(json, strlen(json), chunk);
    RecordingSink sink;
    auto read_fn = [&](uint8_t* buf, size_t max, uint32_t timeout) -> Result<size_t> {
        return feeder.read(buf, max, timeout);
    };
    auto err = sax_parse_streaming(read_fn, 5000, sink);
    REQUIRE(err.empty());
    return sink.events;
}

// ── Branches: SaxStreamBuf minimum size (line 52) ───────────────────────

TEST_CASE("streaming sax: SaxStreamBuf with very small buffer triggers minimum sixth") {
    // Buffer of 30 bytes: sixth=5, which is < 8, so sixth gets clamped to 8
    char storage[30];
    SaxStreamBuf sbuf(storage, 30);
    CHECK(sbuf.rbuf_size == 8);
    CHECK(sbuf.key_size == 8);
    CHECK(sbuf.val_size == 14);  // 30 - 2*8
}

TEST_CASE("streaming sax: SaxStreamBuf with exact 48 bytes (sixth=8 exactly)") {
    char storage[48];
    SaxStreamBuf sbuf(storage, 48);
    CHECK(sbuf.rbuf_size == 8);
    CHECK(sbuf.key_size == 8);
    CHECK(sbuf.val_size == 32);
}

// ── Branches: non-object top level (line 81) ────────────────────────────

TEST_CASE("streaming sax: non-object at top level returns error") {
    auto err = parse_streaming_error("[1,2,3]");
    CHECK(!err.empty());
}

// ── Branches: escape sequences individually (lines 170-177) ────────────

TEST_CASE("streaming sax: escape double-quote") {
    auto events = parse_streaming_ok(R"({"a":"say\"hi\""})", 1);
    CHECK(events[1].value == "say\"hi\"");
}

TEST_CASE("streaming sax: escape backslash individually") {
    auto events = parse_streaming_ok(R"({"a":"a\\b"})", 1);
    CHECK(events[1].value == "a\\b");
}

TEST_CASE("streaming sax: escape forward slash individually") {
    auto events = parse_streaming_ok(R"({"a":"a\/b"})", 1);
    CHECK(events[1].value == "a/b");
}

TEST_CASE("streaming sax: escape backspace") {
    auto events = parse_streaming_ok(R"({"a":"a\bb"})", 1);
    CHECK(events[1].value == "a\bb");
}

TEST_CASE("streaming sax: escape formfeed") {
    auto events = parse_streaming_ok(R"({"a":"a\fb"})", 1);
    CHECK(events[1].value == "a\fb");
}

TEST_CASE("streaming sax: escape carriage return") {
    auto events = parse_streaming_ok(R"({"a":"a\rb"})", 1);
    CHECK(events[1].value == "a\rb");
}

TEST_CASE("streaming sax: escape tab") {
    auto events = parse_streaming_ok(R"({"a":"a\tb"})", 1);
    CHECK(events[1].value == "a\tb");
}

// ── Branches: unicode escape paths (lines 185-200, 209) ────────────────

TEST_CASE("streaming sax: unicode escape uppercase hex") {
    auto events = parse_streaming_ok(R"({"c":"\u0042"})", 1);
    CHECK(events[1].value == "B");
}

TEST_CASE("streaming sax: unicode escape lowercase a-f hex") {
    auto events = parse_streaming_ok(R"({"c":"\u006f"})", 1);
    CHECK(events[1].value == "o");
}

TEST_CASE("streaming sax: unicode 2-byte path detail") {
    auto events = parse_streaming_ok(R"({"c":"\u00C0"})", 1);
    CHECK(events[1].value == "\xC3\x80");
}

TEST_CASE("streaming sax: unicode 3-byte BMP path") {
    auto events = parse_streaming_ok(R"({"c":"\u1234"})", 1);
    CHECK(events[1].value.size() == 3);
    CHECK(events[1].value[0] == '\xE1');
    CHECK(events[1].value[1] == '\x88');
    CHECK(events[1].value[2] == '\xB4');
}

TEST_CASE("streaming sax: unicode surrogate pair D900 replaced with ?") {
    auto events = parse_streaming_ok(R"({"c":"\uD900"})", 1);
    CHECK(events[1].value == "?");
}

TEST_CASE("streaming sax: unicode surrogate DFFF replaced with ?") {
    auto events = parse_streaming_ok(R"({"c":"\uDFFF"})", 1);
    CHECK(events[1].value == "?");
}

TEST_CASE("streaming sax: default escape case (unknown escape outputs literal)") {
    auto events = parse_streaming_ok(R"({"a":"\q"})", 1);
    CHECK(events[1].value == "q");
}

// ── Branches: tab inside string is allowed (line 159) ───────────────────

TEST_CASE("streaming sax: tab character inside string is allowed") {
    std::string json = R"({"a":"hello)";
    json += '\t';
    json += R"(world"})";
    auto events = parse_streaming_ok(json.c_str(), 1);
    CHECK(events[1].value == "hello\tworld");
}

// ── Branches: control character error (line 159-160) ────────────────────

TEST_CASE("streaming sax: control char 0x02 in string") {
    std::string json = R"({"a":"x)";
    json += '\x02';
    json += R"("})";
    auto err = parse_streaming_error(json.c_str(), 1);
    CHECK(!err.empty());
}

// ── Branches: number parsing edge cases ─────────────────────────────────

TEST_CASE("streaming sax: number starting with zero then dot (0.5)") {
    auto events = parse_streaming_ok(R"({"a":0.5})", 1);
    CHECK(events[1].value == "0.5");
}

TEST_CASE("streaming sax: number 0 alone") {
    auto events = parse_streaming_ok(R"({"a":0})", 1);
    CHECK(events[1].value == "0");
}

TEST_CASE("streaming sax: negative float -3.14") {
    auto events = parse_streaming_ok(R"({"a":-3.14})", 1);
    CHECK(events[1].value == "-3.14");
}

TEST_CASE("streaming sax: negative zero -0") {
    auto events = parse_streaming_ok(R"({"a":-0})", 1);
    CHECK(events[1].value == "-0");
}

TEST_CASE("streaming sax: multi-digit integer 12345") {
    auto events = parse_streaming_ok(R"({"a":12345})", 1);
    CHECK(events[1].value == "12345");
}

TEST_CASE("streaming sax: uppercase E exponent") {
    auto events = parse_streaming_ok(R"({"a":2E3})", 1);
    CHECK(events[1].value == "2E3");
}

TEST_CASE("streaming sax: exponent with explicit positive sign") {
    auto events = parse_streaming_ok(R"({"a":5e+1})", 1);
    CHECK(events[1].value == "5e+1");
}

TEST_CASE("streaming sax: decimal then exponent") {
    auto events = parse_streaming_ok(R"({"a":1.5e2})", 1);
    CHECK(events[1].value == "1.5e2");
}

TEST_CASE("streaming sax: decimal then negative exponent") {
    auto events = parse_streaming_ok(R"({"a":3.0e-1})", 1);
    CHECK(events[1].value == "3.0e-1");
}

// ── Branches: number buffer overflow in minus path (line 324-325) ───────

TEST_CASE("streaming sax: negative number with tiny value buffer") {
    const char* json = R"({"a":-999})";
    uint8_t rbuf[32];
    char kbuf[16];
    char vbuf[2];  // very small: only fits '-' and one digit
    SaxStreamBuf sbuf(rbuf, sizeof(rbuf), kbuf, sizeof(kbuf), vbuf, sizeof(vbuf));

    ByteFeeder feeder(json, strlen(json), 256);
    RecordingSink sink;
    auto read_fn = [&](uint8_t* buf, size_t max, uint32_t timeout) -> Result<size_t> {
        return feeder.read(buf, max, timeout);
    };
    auto err = sax_parse_streaming(read_fn, 5000, sbuf, sink);
    REQUIRE(err.empty());
    CHECK(sink.events[1].type == Event::Number);
    CHECK(sink.events[1].value.size() == 2);
}

// ── Branches: number with zero start then exponent (line 331-332) ───────

TEST_CASE("streaming sax: number 0e1 (zero with exponent)") {
    auto events = parse_streaming_ok(R"({"a":0.0e1})", 1);
    CHECK(events[1].value == "0.0e1");
}

// ── Branches: invalid number after minus (line 338-339) ─────────────────

TEST_CASE("streaming sax: minus followed by non-digit") {
    auto err = parse_streaming_error(R"({"a":-a})", 1);
    CHECK(!err.empty());
}

// ── Branches: decimal digit error (line 345-346) ────────────────────────

TEST_CASE("streaming sax: dot followed by non-digit") {
    auto err = parse_streaming_error(R"({"a":1.a})", 1);
    CHECK(!err.empty());
}

// ── Branches: exponent error paths (lines 353-362) ──────────────────────

TEST_CASE("streaming sax: exponent with multi-digit value") {
    auto events = parse_streaming_ok(R"({"a":1e12})", 1);
    CHECK(events[1].value == "1e12");
}

TEST_CASE("streaming sax: uppercase E with negative sign") {
    auto events = parse_streaming_ok(R"({"a":1E-2})", 1);
    CHECK(events[1].value == "1E-2");
}

// ── Branches: exponent digit overflow ───────────────────────────────────

TEST_CASE("streaming sax: exponent digits with tiny buffer triggers else-advance") {
    const char* json = R"({"a":1e999})";
    uint8_t rbuf[32];
    char kbuf[16];
    char vbuf[3];  // only fits "1e" then overflows on exponent digits
    SaxStreamBuf sbuf(rbuf, sizeof(rbuf), kbuf, sizeof(kbuf), vbuf, sizeof(vbuf));

    ByteFeeder feeder(json, strlen(json), 256);
    RecordingSink sink;
    auto read_fn = [&](uint8_t* buf, size_t max, uint32_t timeout) -> Result<size_t> {
        return feeder.read(buf, max, timeout);
    };
    auto err = sax_parse_streaming(read_fn, 5000, sbuf, sink);
    REQUIRE(err.empty());
    CHECK(sink.events[1].type == Event::Number);
    CHECK(sink.events[1].value.size() == 3);
}

// ── Branches: decimal digit overflow with tiny buffer ───────────────────

TEST_CASE("streaming sax: decimal digits with tiny buffer triggers else-advance") {
    const char* json = R"({"a":1.99999})";
    uint8_t rbuf[32];
    char kbuf[16];
    char vbuf[3];  // only fits "1." then overflows
    SaxStreamBuf sbuf(rbuf, sizeof(rbuf), kbuf, sizeof(kbuf), vbuf, sizeof(vbuf));

    ByteFeeder feeder(json, strlen(json), 256);
    RecordingSink sink;
    auto read_fn = [&](uint8_t* buf, size_t max, uint32_t timeout) -> Result<size_t> {
        return feeder.read(buf, max, timeout);
    };
    auto err = sax_parse_streaming(read_fn, 5000, sbuf, sink);
    REQUIRE(err.empty());
    CHECK(sink.events[1].type == Event::Number);
    CHECK(sink.events[1].value.size() == 3);
}

// ── Branches: integer digits with tiny buffer ───────────────────────────

TEST_CASE("streaming sax: integer digits overflow with tiny buffer") {
    const char* json = R"({"a":12345})";
    uint8_t rbuf[32];
    char kbuf[16];
    char vbuf[2];  // only fits "12" then overflows
    SaxStreamBuf sbuf(rbuf, sizeof(rbuf), kbuf, sizeof(kbuf), vbuf, sizeof(vbuf));

    ByteFeeder feeder(json, strlen(json), 256);
    RecordingSink sink;
    auto read_fn = [&](uint8_t* buf, size_t max, uint32_t timeout) -> Result<size_t> {
        return feeder.read(buf, max, timeout);
    };
    auto err = sax_parse_streaming(read_fn, 5000, sbuf, sink);
    REQUIRE(err.empty());
    CHECK(sink.events[1].type == Event::Number);
    CHECK(sink.events[1].value.size() == 2);
}

// ── Branches: dot overflow path (line 343-344) ──────────────────────────

TEST_CASE("streaming sax: dot character overflows tiny buffer") {
    const char* json = R"({"a":1.5})";
    uint8_t rbuf[32];
    char kbuf[16];
    char vbuf[1];  // fits '1', dot overflows
    SaxStreamBuf sbuf(rbuf, sizeof(rbuf), kbuf, sizeof(kbuf), vbuf, 1);

    ByteFeeder feeder(json, strlen(json), 256);
    RecordingSink sink;
    auto read_fn = [&](uint8_t* buf, size_t max, uint32_t timeout) -> Result<size_t> {
        return feeder.read(buf, max, timeout);
    };
    auto err = sax_parse_streaming(read_fn, 5000, sbuf, sink);
    REQUIRE(err.empty());
    CHECK(sink.events[1].type == Event::Number);
    CHECK(sink.events[1].value == "1");
}

// ── Branches: exponent char overflow path (line 354-355) ────────────────

TEST_CASE("streaming sax: exponent char overflows tiny buffer") {
    const char* json = R"({"a":1e2})";
    uint8_t rbuf[32];
    char kbuf[16];
    char vbuf[1];  // fits '1', 'e' overflows
    SaxStreamBuf sbuf(rbuf, sizeof(rbuf), kbuf, sizeof(kbuf), vbuf, 1);

    ByteFeeder feeder(json, strlen(json), 256);
    RecordingSink sink;
    auto read_fn = [&](uint8_t* buf, size_t max, uint32_t timeout) -> Result<size_t> {
        return feeder.read(buf, max, timeout);
    };
    auto err = sax_parse_streaming(read_fn, 5000, sbuf, sink);
    REQUIRE(err.empty());
    CHECK(sink.events[1].type == Event::Number);
    CHECK(sink.events[1].value == "1");
}

// ── Branches: exponent sign overflow (line 356-358) ─────────────────────

TEST_CASE("streaming sax: exponent sign overflows tiny buffer") {
    const char* json = R"({"a":1e-2})";
    uint8_t rbuf[32];
    char kbuf[16];
    char vbuf[2];  // fits '1' and 'e', sign '-' overflows
    SaxStreamBuf sbuf(rbuf, sizeof(rbuf), kbuf, sizeof(kbuf), vbuf, 2);

    ByteFeeder feeder(json, strlen(json), 256);
    RecordingSink sink;
    auto read_fn = [&](uint8_t* buf, size_t max, uint32_t timeout) -> Result<size_t> {
        return feeder.read(buf, max, timeout);
    };
    auto err = sax_parse_streaming(read_fn, 5000, sbuf, sink);
    REQUIRE(err.empty());
    CHECK(sink.events[1].type == Event::Number);
    CHECK(sink.events[1].value == "1e");
}

// ── Branches: EOF in number at various points ───────────────────────────

TEST_CASE("streaming sax: EOF after decimal point") {
    auto err = parse_streaming_error(R"({"a":1.)", 1);
    CHECK(!err.empty());
}

TEST_CASE("streaming sax: EOF after exponent sign") {
    auto err = parse_streaming_error(R"({"a":1e+)", 1);
    CHECK(!err.empty());
}

TEST_CASE("streaming sax: EOF after exponent E") {
    auto err = parse_streaming_error(R"({"a":1e)", 1);
    CHECK(!err.empty());
}

// ── Branches: error during null literal (line 313-314) ──────────────────

TEST_CASE("streaming sax: null literal truncated at 2 chars") {
    auto err = parse_streaming_error(R"({"a":nu)", 1);
    CHECK(!err.empty());
}

TEST_CASE("streaming sax: null literal mismatch") {
    auto err = parse_streaming_error(R"({"a":nxll})", 1);
    CHECK(!err.empty());
}

// ── Branches: true literal truncated (line 303) ────────────────────────

TEST_CASE("streaming sax: true literal truncated at 1 char") {
    auto err = parse_streaming_error(R"({"a":t)", 1);
    CHECK(!err.empty());
}

TEST_CASE("streaming sax: true literal mismatch") {
    auto err = parse_streaming_error(R"({"a":trxe})", 1);
    CHECK(!err.empty());
}

// ── Branches: false literal errors ──────────────────────────────────────

TEST_CASE("streaming sax: false literal truncated") {
    auto err = parse_streaming_error(R"({"a":fal)", 1);
    CHECK(!err.empty());
}

TEST_CASE("streaming sax: false literal mismatch") {
    auto err = parse_streaming_error(R"({"a":faxse})", 1);
    CHECK(!err.empty());
}

// ── Branches: array missing comma (line 272) ────────────────────────────

TEST_CASE("streaming sax: array missing comma between elements") {
    auto err = parse_streaming_error(R"({"a":[1 2]})", 1);
    CHECK(!err.empty());
}

// ── Branches: unicode scratch overflow paths ────────────────────────────

TEST_CASE("streaming sax: unicode 2-byte overflows scratch") {
    const char* json = R"({"a":"\u00E9"})";
    uint8_t rbuf[32];
    char kbuf[16];
    char vbuf[1];  // out+1 < scratch_size check fails for 2-byte char
    SaxStreamBuf sbuf(rbuf, sizeof(rbuf), kbuf, sizeof(kbuf), vbuf, 1);

    ByteFeeder feeder(json, strlen(json), 256);
    RecordingSink sink;
    auto read_fn = [&](uint8_t* buf, size_t max, uint32_t timeout) -> Result<size_t> {
        return feeder.read(buf, max, timeout);
    };
    auto err = sax_parse_streaming(read_fn, 5000, sbuf, sink);
    REQUIRE(err.empty());
    CHECK(sink.events[1].value.empty());
}

TEST_CASE("streaming sax: unicode 3-byte overflows scratch") {
    const char* json = R"({"a":"\u4E16"})";
    uint8_t rbuf[32];
    char kbuf[16];
    char vbuf[2];  // out+2 < scratch_size check fails for 3-byte
    SaxStreamBuf sbuf(rbuf, sizeof(rbuf), kbuf, sizeof(kbuf), vbuf, 2);

    ByteFeeder feeder(json, strlen(json), 256);
    RecordingSink sink;
    auto read_fn = [&](uint8_t* buf, size_t max, uint32_t timeout) -> Result<size_t> {
        return feeder.read(buf, max, timeout);
    };
    auto err = sax_parse_streaming(read_fn, 5000, sbuf, sink);
    REQUIRE(err.empty());
    CHECK(sink.events[1].value.empty());
}

TEST_CASE("streaming sax: unicode ASCII overflows 1-byte scratch") {
    const char* json = R"({"a":"x\u0041"})";
    uint8_t rbuf[32];
    char kbuf[16];
    char vbuf[1];  // fits 'x' but then \u0041 overflows
    SaxStreamBuf sbuf(rbuf, sizeof(rbuf), kbuf, sizeof(kbuf), vbuf, 1);

    ByteFeeder feeder(json, strlen(json), 256);
    RecordingSink sink;
    auto read_fn = [&](uint8_t* buf, size_t max, uint32_t timeout) -> Result<size_t> {
        return feeder.read(buf, max, timeout);
    };
    auto err = sax_parse_streaming(read_fn, 5000, sbuf, sink);
    REQUIRE(err.empty());
    CHECK(sink.events[1].value == "x");  // only 'x' fits
}

// ── Branches: string character overflow ─────────────────────────────────

TEST_CASE("streaming sax: string char overflows scratch silently") {
    const char* json = R"({"a":"hello"})";
    uint8_t rbuf[32];
    char kbuf[16];
    char vbuf[3];  // only fits "hel"
    SaxStreamBuf sbuf(rbuf, sizeof(rbuf), kbuf, sizeof(kbuf), vbuf, 3);

    ByteFeeder feeder(json, strlen(json), 256);
    RecordingSink sink;
    auto read_fn = [&](uint8_t* buf, size_t max, uint32_t timeout) -> Result<size_t> {
        return feeder.read(buf, max, timeout);
    };
    auto err = sax_parse_streaming(read_fn, 5000, sbuf, sink);
    REQUIRE(err.empty());
    CHECK(sink.events[1].value == "hel");
}

// ── Branches: escape character overflow ─────────────────────────────────

TEST_CASE("streaming sax: escape char overflows scratch") {
    const char* json = R"({"a":"x\n"})";
    uint8_t rbuf[32];
    char kbuf[16];
    char vbuf[1];  // fits 'x' but then \n overflows
    SaxStreamBuf sbuf(rbuf, sizeof(rbuf), kbuf, sizeof(kbuf), vbuf, 1);

    ByteFeeder feeder(json, strlen(json), 256);
    RecordingSink sink;
    auto read_fn = [&](uint8_t* buf, size_t max, uint32_t timeout) -> Result<size_t> {
        return feeder.read(buf, max, timeout);
    };
    auto err = sax_parse_streaming(read_fn, 5000, sbuf, sink);
    REQUIRE(err.empty());
    CHECK(sink.events[1].value == "x");
}

// ── Branches: expected '"' error (line 143) ─────────────────────────────

TEST_CASE("streaming sax: key position gets non-quote character") {
    auto err = parse_streaming_error(R"({123:1})", 1);
    CHECK(!err.empty());
}

// ── Branches: whitespace characters \r and \t in JSON structure ─────────

TEST_CASE("streaming sax: CR and tab as structural whitespace") {
    std::string json = "{\r\n\t\"a\"\t:\r\n1\r\n}";
    auto events = parse_streaming_ok(json.c_str(), 1);
    REQUIRE(events.size() == 3);
    CHECK(events[1].key == "a");
    CHECK(events[1].value == "1");
}

// ── Branches: deeply nested 5+ levels ───────────────────────────────────

TEST_CASE("streaming sax: 6 levels of nesting") {
    const char* json = R"({"a":{"b":{"c":{"d":{"e":{"f":1}}}}}})";
    auto events = parse_streaming_ok(json, 1);
    REQUIRE(events.size() == 13);
    CHECK(events[6].type == Event::Number);
    CHECK(events[6].key == "f");
    CHECK(events[6].value == "1");
}

// ── Branches: array of mixed types ──────────────────────────────────────

TEST_CASE("streaming sax: array of mixed types") {
    const char* json = R"({"a":[1,"two",true,null,3.14]})";
    auto events = parse_streaming_ok(json, 1);
    CHECK(events[2].type == Event::Number);
    CHECK(events[2].value == "1");
    CHECK(events[3].type == Event::String);
    CHECK(events[3].value == "two");
    CHECK(events[4].type == Event::Bool);
    CHECK(events[4].value == "true");
    CHECK(events[5].type == Event::Null);
    CHECK(events[6].type == Event::Number);
    CHECK(events[6].value == "3.14");
}

// ── Branches: nested array with objects ─────────────────────────────────

TEST_CASE("streaming sax: array of objects") {
    const char* json = R"({"a":[{"x":1},{"y":2}]})";
    auto events = parse_streaming_ok(json, 1);
    CHECK(events[0].type == Event::ObjBegin);
    CHECK(events[1].type == Event::ArrBegin);
    CHECK(events[2].type == Event::ObjBegin);
    CHECK(events[3].type == Event::Number);
    CHECK(events[3].key == "x");
    CHECK(events[4].type == Event::ObjEnd);
    CHECK(events[5].type == Event::ObjBegin);
    CHECK(events[6].type == Event::Number);
    CHECK(events[6].key == "y");
    CHECK(events[7].type == Event::ObjEnd);
    CHECK(events[8].type == Event::ArrEnd);
    CHECK(events[9].type == Event::ObjEnd);
}
