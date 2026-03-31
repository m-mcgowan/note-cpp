// Tests for StreamingJsonBuilder, JsonBufferWriter, CrcWriter,
// and the incremental CRC32 API.

#include "catch.hpp"

#include <note/backends/buffer.hpp>
#include <note/json.hpp>
#include <note/transport/detail/crc32.hpp>
#include <note/transport.hpp>

#include <cstring>
#include <string>
#include <vector>

using namespace note;
using namespace note::transport::detail;

// ---------------------------------------------------------------------------
// Incremental CRC32
// ---------------------------------------------------------------------------

TEST_CASE("crc32_update: incremental matches single-shot") {
    const char* data = "123456789";
    size_t len = strlen(data);

    uint32_t expected = crc32(data, len);

    // Accumulate in chunks
    uint32_t state = crc32_begin();
    state = crc32_update(state, data, 3);      // "123"
    state = crc32_update(state, data + 3, 3);  // "456"
    state = crc32_update(state, data + 6, 3);  // "789"
    uint32_t actual = crc32_finalize(state);

    REQUIRE(actual == expected);
}

TEST_CASE("crc32_update: single-byte increments") {
    const char* data = "hello";
    size_t len = strlen(data);
    uint32_t expected = crc32(data, len);

    uint32_t state = crc32_begin();
    for (size_t i = 0; i < len; ++i)
        state = crc32_update(state, data + i, 1);
    uint32_t actual = crc32_finalize(state);

    REQUIRE(actual == expected);
}

TEST_CASE("crc32_update: empty input") {
    uint32_t expected = crc32("", 0);
    uint32_t state = crc32_begin();
    uint32_t actual = crc32_finalize(state);
    REQUIRE(actual == expected);
}

TEST_CASE("crc32_update: JSON request matches single-shot") {
    const char* json = R"({"req":"card.status"})";
    size_t len = strlen(json);
    uint32_t expected = crc32(json, len);

    uint32_t state = crc32_begin();
    state = crc32_update(state, json, len);
    uint32_t actual = crc32_finalize(state);

    REQUIRE(actual == expected);
}

// ---------------------------------------------------------------------------
// JsonBufferWriter
// ---------------------------------------------------------------------------

TEST_CASE("JsonBufferWriter: basic write") {
    char buf[64];
    JsonBufferWriter w(buf, sizeof(buf));
    w.write("hello", 5);
    REQUIRE(w.pos() == 5);
    REQUIRE(w.view() == "hello");
    REQUIRE_FALSE(w.overflow());
}

TEST_CASE("JsonBufferWriter: overflow tracking") {
    char buf[4];
    JsonBufferWriter w(buf, sizeof(buf));
    w.write("hello", 5);
    REQUIRE(w.overflow());
    REQUIRE(w.pos() == 5);
    REQUIRE(w.view() == "hell");  // clamped to capacity
}

TEST_CASE("JsonBufferWriter: reset") {
    char buf[64];
    JsonBufferWriter w(buf, sizeof(buf));
    w.write("first", 5);
    w.reset();
    w.write("ab", 2);
    REQUIRE(w.pos() == 2);
    REQUIRE(w.view() == "ab");
}

TEST_CASE("JsonBufferWriter: single char write") {
    char buf[16];
    JsonBufferWriter w(buf, sizeof(buf));
    w.write('{');
    w.write('}');
    REQUIRE(w.view() == "{}");
}

// ---------------------------------------------------------------------------
// StreamingJsonBuilder — output must match BufferJsonBuilder exactly
// ---------------------------------------------------------------------------

namespace {

// Helper: build JSON with StreamingJsonBuilder, return as std::string.
template<typename BuildFn>
std::string streaming_build(BuildFn fn) {
    char buf[512];
    JsonBufferWriter w(buf, sizeof(buf));
    StreamingJsonBuilder b(w);
    fn(b);
    b.to_view();  // close
    return std::string(w.view());
}

// Helper: build JSON with BufferJsonBuilder, return as std::string.
template<typename BuildFn>
std::string buffer_build(BuildFn fn) {
    char buf[512];
    note::backends::BufferJsonBuilder b(buf, sizeof(buf));
    fn(b);
    return std::string(b.to_view());
}

} // anonymous namespace

TEST_CASE("StreamingJsonBuilder: empty object") {
    auto fn = [](JsonBuilder&) {};
    REQUIRE(streaming_build(fn) == buffer_build(fn));
    REQUIRE(streaming_build(fn) == "{}");
}

TEST_CASE("StreamingJsonBuilder: basic types match BufferJsonBuilder") {
    auto fn = [](JsonBuilder& b) {
        b.add("flag", true);
        b.add("count", int32_t{42});
        b.add("temp", 22.5);
        b.add("name", string_view("hello"));
    };
    REQUIRE(streaming_build(fn) == buffer_build(fn));
}

TEST_CASE("StreamingJsonBuilder: false boolean") {
    auto fn = [](JsonBuilder& b) { b.add("off", false); };
    REQUIRE(streaming_build(fn) == buffer_build(fn));
}

TEST_CASE("StreamingJsonBuilder: negative integer") {
    auto fn = [](JsonBuilder& b) { b.add("val", int32_t{-100}); };
    REQUIRE(streaming_build(fn) == buffer_build(fn));
}

TEST_CASE("StreamingJsonBuilder: nested object") {
    auto fn = [](JsonBuilder& b) {
        b.add("req", string_view("note.add"));
        b.begin_object("body");
        b.add("temp", 22.5);
        b.add("label", string_view("room"));
        b.end_object();
    };
    REQUIRE(streaming_build(fn) == buffer_build(fn));
}

TEST_CASE("StreamingJsonBuilder: array with elements") {
    auto fn = [](JsonBuilder& b) {
        b.begin_array("files");
        b.add_element(string_view("data.qi"));
        b.add_element(string_view("settings.db"));
        b.end_array();
    };
    REQUIRE(streaming_build(fn) == buffer_build(fn));
}

TEST_CASE("StreamingJsonBuilder: mixed array types") {
    auto fn = [](JsonBuilder& b) {
        b.begin_array("items");
        b.add_element(true);
        b.add_element(int32_t{7});
        b.add_element(3.14);
        b.add_element(string_view("text"));
        b.end_array();
    };
    REQUIRE(streaming_build(fn) == buffer_build(fn));
}

TEST_CASE("StreamingJsonBuilder: string escaping") {
    auto fn = [](JsonBuilder& b) {
        b.add("msg", string_view("line1\nline2\ttab\"quote\\backslash"));
    };
    REQUIRE(streaming_build(fn) == buffer_build(fn));
}

TEST_CASE("StreamingJsonBuilder: raw JSON fragment") {
    auto fn = [](JsonBuilder& b) {
        b.add_raw("body", string_view(R"({"nested":true})"));
    };
    REQUIRE(streaming_build(fn) == buffer_build(fn));
}

TEST_CASE("StreamingJsonBuilder: add with const char*") {
    auto fn = [](JsonBuilder& b) {
        b.add("key", "value");  // const char* → string_view overload
    };
    REQUIRE(streaming_build(fn) == buffer_build(fn));
}

// ---------------------------------------------------------------------------
// CrcWriter (nested in AbstractTransport)
// ---------------------------------------------------------------------------

TEST_CASE("CrcWriter: CRC matches single-shot crc32") {
    // Build {"req":"card.status"} through CrcWriter and check that the
    // accumulated CRC matches crc32() on the same bytes.
    const char* json = R"({"req":"card.status"})";
    size_t len = strlen(json);
    uint32_t expected = crc32(json, len);

    char buf[256];
    JsonBufferWriter bw(buf, sizeof(buf));
    AbstractTransport::CrcWriter cw(bw);

    // Write everything except closing }
    cw.write(json, len - 1);
    uint32_t actual = cw.finalize_with_brace();

    REQUIRE(actual == expected);
    // Buffer should contain json without closing }
    REQUIRE(bw.view() == string_view(json, len - 1));
}

TEST_CASE("CrcWriter: streaming build CRC matches crc_add") {
    // Build the same request both ways and verify CRC values match.
    // Method 1: BufferJsonBuilder + crc_add (existing approach)
    char buf1[256];
    note::backends::BufferJsonBuilder builder(buf1, sizeof(buf1));
    builder.add("req", string_view("card.status"));
    auto json = builder.to_view();
    char wire1[256];
    memcpy(wire1, json.data(), json.size());
    size_t wire1_len = crc_add(wire1, json.size(), sizeof(wire1), 1);

    // Method 2: StreamingJsonBuilder + CrcWriter
    char buf2[256];
    JsonBufferWriter bw(buf2, sizeof(buf2));
    AbstractTransport::CrcWriter cw(bw);
    StreamingJsonBuilder sbuilder(cw);
    sbuilder.add("req", string_view("card.status"));
    uint32_t checksum = cw.finalize_with_brace();

    // Manually append CRC field + } to buf2
    size_t pos = bw.pos();
    buf2[pos++] = ',';
    buf2[pos++] = '"'; buf2[pos++] = 'c'; buf2[pos++] = 'r'; buf2[pos++] = 'c';
    buf2[pos++] = '"'; buf2[pos++] = ':'; buf2[pos++] = '"';
    write_hex16(buf2 + pos, 1); pos += 4;
    buf2[pos++] = ':';
    write_hex32(buf2 + pos, checksum); pos += 8;
    buf2[pos++] = '"';
    buf2[pos++] = '}';

    // Both methods should produce identical wire format
    REQUIRE(string_view(wire1, wire1_len) == string_view(buf2, pos));
}

TEST_CASE("CrcWriter: round-trip with crc_check_and_strip") {
    // Build with CrcWriter, verify with crc_check_and_strip.
    char buf[256];
    JsonBufferWriter bw(buf, sizeof(buf));
    AbstractTransport::CrcWriter cw(bw);
    StreamingJsonBuilder builder(cw);
    builder.add("req", string_view("note.add"));
    builder.begin_object("body");
    builder.add("temp", 22.5);
    builder.end_object();
    uint32_t checksum = cw.finalize_with_brace();

    size_t pos = bw.pos();
    buf[pos++] = ',';
    buf[pos++] = '"'; buf[pos++] = 'c'; buf[pos++] = 'r'; buf[pos++] = 'c';
    buf[pos++] = '"'; buf[pos++] = ':'; buf[pos++] = '"';
    write_hex16(buf + pos, 5); pos += 4;
    buf[pos++] = ':';
    write_hex32(buf + pos, checksum); pos += 8;
    buf[pos++] = '"';
    buf[pos++] = '}';

    // Verify via crc_check_and_strip
    bool crc_enabled = false;
    bool error = crc_check_and_strip(buf, pos, 5, crc_enabled);
    REQUIRE_FALSE(error);
    REQUIRE(crc_enabled);

    // Stripped result should be the original JSON
    auto expected_json = buffer_build([](JsonBuilder& b) {
        b.add("req", string_view("note.add"));
        b.begin_object("body");
        b.add("temp", 22.5);
        b.end_object();
    });
    REQUIRE(string_view(buf, pos) == expected_json);
}

// ---------------------------------------------------------------------------
// FilterJsonSink + reset
// ---------------------------------------------------------------------------

TEST_CASE("FilterJsonSink: forwards all events") {
    struct Recorder : JsonSink {
        int strings = 0, bools = 0, numbers = 0, nulls = 0;
        int obj_begin = 0, obj_end = 0, arr_begin = 0, arr_end = 0;
        int resets = 0;

        void on_null(string_view) override { ++nulls; }
        void on_bool(string_view, bool) override { ++bools; }
        void on_number(string_view, string_view) override { ++numbers; }
        void on_string(string_view, string_view) override { ++strings; }
        void on_object_begin(string_view) override { ++obj_begin; }
        void on_object_end(string_view) override { ++obj_end; }
        void on_array_begin(string_view) override { ++arr_begin; }
        void on_array_end(string_view) override { ++arr_end; }
        void reset() override { ++resets; }
    };

    Recorder rec;
    FilterJsonSink filter(rec);

    filter.on_null("k");
    filter.on_bool("k", true);
    filter.on_number("k", "42");
    filter.on_string("k", "v");
    filter.on_object_begin("k");
    filter.on_object_end("k");
    filter.on_array_begin("k");
    filter.on_array_end("k");
    filter.reset();

    REQUIRE(rec.nulls == 1);
    REQUIRE(rec.bools == 1);
    REQUIRE(rec.numbers == 1);
    REQUIRE(rec.strings == 1);
    REQUIRE(rec.obj_begin == 1);
    REQUIRE(rec.obj_end == 1);
    REQUIRE(rec.arr_begin == 1);
    REQUIRE(rec.arr_end == 1);
    REQUIRE(rec.resets == 1);
}

TEST_CASE("ErrorCaptureSink: intercepts err, forwards rest") {
    struct Recorder : JsonSink {
        std::vector<std::string> keys;
        void on_string(string_view k, string_view) override {
            keys.emplace_back(k.data(), k.size());
        }
    };

    Recorder rec;
    ErrorCaptureSink err(rec);

    err.on_string("name", "alice");
    err.on_string("err", "something broke");
    err.on_string("status", "ok");

    REQUIRE(err.captured_error() == "something broke");
    REQUIRE(rec.keys.size() == 2);
    REQUIRE(rec.keys[0] == "name");
    REQUIRE(rec.keys[1] == "status");
}

TEST_CASE("ErrorCaptureSink: reset clears error") {
    JsonSink null_sink;
    ErrorCaptureSink err(null_sink);

    err.on_string("err", "fail");
    REQUIRE_FALSE(err.captured_error().empty());

    err.reset();
    REQUIRE(err.captured_error().empty());
}

// ---------------------------------------------------------------------------
// CrcFieldSink
// ---------------------------------------------------------------------------

TEST_CASE("CrcFieldSink: intercepts crc field, forwards rest") {
    struct Recorder : JsonSink {
        std::vector<std::string> keys;
        void on_string(string_view k, string_view) override {
            keys.emplace_back(k.data(), k.size());
        }
        void on_bool(string_view k, bool) override {
            keys.emplace_back(k.data(), k.size());
        }
    };

    Recorder rec;
    AbstractTransport::CrcFieldSink crc(rec);

    crc.on_string("status", "ok");
    crc.on_bool("connected", true);
    crc.on_string("crc", "0001:ABCDEF01");
    crc.on_string("name", "test");

    REQUIRE(crc.has_crc());
    REQUIRE(crc.seq() == 1);
    REQUIRE(crc.checksum() == 0xABCDEF01);
    REQUIRE(rec.keys.size() == 3);
    REQUIRE(rec.keys[0] == "status");
    REQUIRE(rec.keys[1] == "connected");
    REQUIRE(rec.keys[2] == "name");
}

TEST_CASE("CrcFieldSink: no crc field") {
    JsonSink null_sink;
    AbstractTransport::CrcFieldSink crc(null_sink);

    crc.on_string("status", "ok");

    REQUIRE_FALSE(crc.has_crc());
}

TEST_CASE("CrcFieldSink: reset clears state") {
    JsonSink null_sink;
    AbstractTransport::CrcFieldSink crc(null_sink);

    crc.on_string("crc", "0005:12345678");
    REQUIRE(crc.has_crc());

    crc.reset();
    REQUIRE_FALSE(crc.has_crc());
}

// ---------------------------------------------------------------------------
// Streaming receive integration (transport-level)
// ---------------------------------------------------------------------------

namespace {

// Minimal concrete transport for testing streaming receive.
struct TestStreamTransport : AbstractTransport {
    // Expose protected members for test access
    using AbstractTransport::initialized_;
    using AbstractTransport::crc_enabled_;
    using AbstractTransport::crc_seq_;

    std::string response;
    size_t read_pos = 0;
    size_t chunk_size = 64;

    bool do_transmit(const char*, size_t) override { return true; }
#ifndef NOTE_NO_STD_STRING
    Result<void> do_receive(std::string&, uint32_t) override {
        return make_error(Error::NotReady, "use streaming");
    }
#endif
    bool do_reset() override { return true; }
    Result<size_t> do_read(uint8_t* buf, size_t max, uint32_t) override {
        if (read_pos >= response.size()) return size_t(0);
        size_t n = std::min({max, chunk_size, response.size() - read_pos});
        memcpy(buf, response.data() + read_pos, n);
        read_pos += n;
        return n;
    }
    uint32_t max_retries() const override { return 0; }
    uint32_t retry_delay_ms() const override { return 0; }
    void delay(uint32_t) override {}

    void set_response(const char* json) {
        response = json;
        response += '\n';
        read_pos = 0;
    }

    /// Build response with CRC. seq is the value BEFORE transact_streaming
    /// increments — the response CRC uses seq+1 to match.
    void set_response_with_crc(const char* json, uint16_t seq) {
        char buf[512];
        size_t len = strlen(json);
        memcpy(buf, json, len);
        uint16_t wire_seq = static_cast<uint16_t>(seq + 1);
        len = transport::detail::crc_add(buf, len, sizeof(buf), wire_seq);
        response.assign(buf, len);
        response += '\n';
        read_pos = 0;
        crc_enabled_ = true;
        crc_seq_ = seq;
    }
};

} // anonymous namespace

TEST_CASE("transact_streaming: basic SAX parse") {
    TestStreamTransport transport;
    transport.set_response(R"({"status":"ok","temp":22.5})");
    transport.initialized_ = true;

    struct Sink : JsonSink {
        std::string status;
        double temp = 0;
        void on_string(string_view k, string_view v) override {
            if (k == "status") status = std::string(v.data(), v.size());
        }
        void on_number(string_view k, string_view raw) override {
            if (k == "temp") temp = parse_double(raw);
        }
    } sink;

    auto rv = transport.transact_streaming([](JsonBuilder& b) {
        b.add("req", "test.run");
    }, sink, 1000);
    REQUIRE(rv.has_value());
    REQUIRE(sink.status == "ok");
    REQUIRE(sink.temp == Approx(22.5));
}

TEST_CASE("transact_streaming: CRC verification passes") {
    TestStreamTransport transport;
    transport.set_response_with_crc(R"({"val":42})", 0);
    transport.initialized_ = true;

    struct Sink : JsonSink {
        int32_t val = 0;
        void on_number(string_view k, string_view raw) override {
            if (k == "val") val = parse_int(raw);
        }
    } sink;

    auto rv = transport.transact_streaming([](JsonBuilder& b) {
        b.add("req", "test.run");
    }, sink, 1000);
    REQUIRE(rv.has_value());
    REQUIRE(sink.val == 42);
}

TEST_CASE("transact_streaming: CRC mismatch detected") {
    TestStreamTransport transport;
    transport.response = R"({"val":42,"crc":"0001:DEADBEEF"})";
    transport.response += '\n';
    transport.crc_enabled_ = true;
    transport.crc_seq_ = 0;
    transport.initialized_ = true;

    JsonSink null_sink;
    auto rv = transport.transact_streaming([](JsonBuilder& b) {
        b.add("req", "test.run");
    }, null_sink, 1000);
    REQUIRE_FALSE(rv.has_value());
    REQUIRE(rv.error().code == Error::ResponseLost);
}

TEST_CASE("transact_streaming: send + receive round trip") {
    TestStreamTransport transport;
    transport.set_response(R"({"result":"done"})");
    transport.initialized_ = true;

    struct Sink : JsonSink {
        std::string result;
        void on_string(string_view k, string_view v) override {
            if (k == "result") result = std::string(v.data(), v.size());
        }
    } sink;

    auto rv = transport.transact_streaming([](JsonBuilder& b) {
        b.add("req", "test.run");
    }, sink, 1000);

    REQUIRE(rv.has_value());
    REQUIRE(sink.result == "done");
}
