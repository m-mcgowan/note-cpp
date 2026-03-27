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
