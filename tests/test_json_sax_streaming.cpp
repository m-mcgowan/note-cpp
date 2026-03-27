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
