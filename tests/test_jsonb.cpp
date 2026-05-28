// Tests for JSONB opcode constants, StreamingJsonbBuilder, parser,
// and COBS stream writer.

#include <doctest.h>

#include <note/jsonb.hpp>
#include <note/json_sax_streaming.hpp>
#include <note/lexer/sax_adapter.hpp>
#include <note/link/cobs.hpp>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

using namespace note;

// ---------------------------------------------------------------------------
// Helpers — capture raw opcode bytes from the builder
// ---------------------------------------------------------------------------

namespace {

// Collect bytes written by the builder into a vector.
struct ByteCapture : JsonWriter {
    std::vector<uint8_t> bytes;

    bool write(const char* data, size_t len) override {
        for (size_t i = 0; i < len; ++i)
            bytes.push_back(static_cast<uint8_t>(data[i]));
        return true;
    }
};

// Build JSONB opcodes and return the raw byte stream.
template<typename BuildFn>
std::vector<uint8_t> jsonb_build(BuildFn fn) {
    ByteCapture cap;
    StreamingJsonbBuilder b(cap);
    fn(b);
    b.to_view();  // close root object
    return cap.bytes;
}

// Build JSONB opcodes WITHOUT closing (simulates streaming transport path
// where the transport handles kEndObject).
template<typename BuildFn>
std::vector<uint8_t> jsonb_build_open(BuildFn fn) {
    ByteCapture cap;
    StreamingJsonbBuilder b(cap);
    fn(b);
    return cap.bytes;
}

}  // anonymous namespace

// ---------------------------------------------------------------------------
// Opcode constants
// ---------------------------------------------------------------------------

TEST_CASE("jsonb: opcode constants match note-c-zero") {
    CHECK(jsonb::kBeginObject == 0x10);
    CHECK(jsonb::kEndObject   == 0x11);
    CHECK(jsonb::kBeginArray  == 0x12);
    CHECK(jsonb::kEndArray    == 0x13);
    CHECK(jsonb::kNull        == 0x20);
    CHECK(jsonb::kTrue        == 0x21);
    CHECK(jsonb::kFalse       == 0x22);
    CHECK(jsonb::kItem        == 0x30);
    CHECK(jsonb::kString      == 0x40);
    CHECK(jsonb::kInt32       == 0x64);
    CHECK(jsonb::kDouble      == 0x88);
    CHECK(jsonb::kCobsXor     == '\n');
}

// ---------------------------------------------------------------------------
// StreamingJsonbBuilder — opcode stream verification
// ---------------------------------------------------------------------------

TEST_CASE("jsonb builder: empty object") {
    auto bytes = jsonb_build([](JsonBuilder&) {});
    // kBeginObject + kEndObject
    REQUIRE(bytes.size() == 2);
    CHECK(bytes[0] == jsonb::kBeginObject);
    CHECK(bytes[1] == jsonb::kEndObject);
}

TEST_CASE("jsonb builder: single string field") {
    auto bytes = jsonb_build([](JsonBuilder& b) {
        b.add("req", string_view("card.version"));
    });

    // Expected: kBeginObject
    //           kItem + "req\0"
    //           kString + "card.version\0"
    //           kEndObject
    size_t i = 0;
    REQUIRE(bytes.size() > 0);
    CHECK(bytes[i++] == jsonb::kBeginObject);

    CHECK(bytes[i++] == jsonb::kItem);
    CHECK(bytes[i] == 'r'); i++;
    CHECK(bytes[i] == 'e'); i++;
    CHECK(bytes[i] == 'q'); i++;
    CHECK(bytes[i] == '\0'); i++;

    CHECK(bytes[i++] == jsonb::kString);
    CHECK(memcmp(&bytes[i], "card.version", 12) == 0); i += 12;
    CHECK(bytes[i] == '\0'); i++;

    CHECK(bytes[i++] == jsonb::kEndObject);
    REQUIRE(i == bytes.size());
}

TEST_CASE("jsonb builder: int32 field") {
    auto bytes = jsonb_build([](JsonBuilder& b) {
        b.add("count", int32_t{42});
    });

    size_t i = 0;
    CHECK(bytes[i++] == jsonb::kBeginObject);
    // kItem + "count\0"
    CHECK(bytes[i++] == jsonb::kItem);
    CHECK(memcmp(&bytes[i], "count", 5) == 0); i += 5;
    CHECK(bytes[i++] == '\0');
    // kInt32 + 42 in little-endian
    CHECK(bytes[i++] == jsonb::kInt32);
    CHECK(bytes[i++] == 42);   // LSB
    CHECK(bytes[i++] == 0);
    CHECK(bytes[i++] == 0);
    CHECK(bytes[i++] == 0);    // MSB
    CHECK(bytes[i++] == jsonb::kEndObject);
    REQUIRE(i == bytes.size());
}

TEST_CASE("jsonb builder: negative int32") {
    auto bytes = jsonb_build([](JsonBuilder& b) {
        b.add("val", int32_t{-1});
    });

    // Find the int32 payload (after kItem + "val\0" + kInt32)
    // kBeginObject + kItem + "val\0" + kInt32 + 4 bytes + kEndObject
    size_t i = 1 + 1 + 4 + 1;  // after kInt32 opcode
    CHECK(bytes[i++] == 0xFF);  // -1 in LE = FF FF FF FF
    CHECK(bytes[i++] == 0xFF);
    CHECK(bytes[i++] == 0xFF);
    CHECK(bytes[i++] == 0xFF);
}

TEST_CASE("jsonb builder: double field") {
    auto bytes = jsonb_build([](JsonBuilder& b) {
        b.add("temp", 22.5);
    });

    size_t i = 0;
    CHECK(bytes[i++] == jsonb::kBeginObject);
    // Skip kItem + "temp\0"
    CHECK(bytes[i++] == jsonb::kItem);
    i += 5;  // "temp\0"
    CHECK(bytes[i++] == jsonb::kDouble);
    // Verify the 8 bytes match the IEEE 754 representation of 22.5
    double expected = 22.5;
    CHECK(memcmp(&bytes[i], &expected, 8) == 0);
    i += 8;
    CHECK(bytes[i++] == jsonb::kEndObject);
    REQUIRE(i == bytes.size());
}

TEST_CASE("jsonb builder: bool fields") {
    auto bytes = jsonb_build([](JsonBuilder& b) {
        b.add("on", true);
        b.add("off", false);
    });

    size_t i = 0;
    CHECK(bytes[i++] == jsonb::kBeginObject);

    // "on" = true
    CHECK(bytes[i++] == jsonb::kItem);
    i += 3;  // "on\0"
    CHECK(bytes[i++] == jsonb::kTrue);

    // "off" = false
    CHECK(bytes[i++] == jsonb::kItem);
    i += 4;  // "off\0"
    CHECK(bytes[i++] == jsonb::kFalse);

    CHECK(bytes[i++] == jsonb::kEndObject);
    REQUIRE(i == bytes.size());
}

TEST_CASE("jsonb builder: nested object") {
    auto bytes = jsonb_build([](JsonBuilder& b) {
        b.add("req", string_view("note.add"));
        b.begin_object("body");
        b.add("temp", 22.5);
        b.end_object();
    });

    size_t i = 0;
    CHECK(bytes[i++] == jsonb::kBeginObject);

    // "req" = "note.add"
    CHECK(bytes[i++] == jsonb::kItem);
    i += 4;  // "req\0"
    CHECK(bytes[i++] == jsonb::kString);
    i += 9;  // "note.add\0"

    // begin_object("body")
    CHECK(bytes[i++] == jsonb::kItem);
    i += 5;  // "body\0"
    CHECK(bytes[i++] == jsonb::kBeginObject);

    // "temp" = 22.5
    CHECK(bytes[i++] == jsonb::kItem);
    i += 5;  // "temp\0"
    CHECK(bytes[i++] == jsonb::kDouble);
    i += 8;  // 8 bytes of double

    CHECK(bytes[i++] == jsonb::kEndObject);  // close body
    CHECK(bytes[i++] == jsonb::kEndObject);  // close root
    REQUIRE(i == bytes.size());
}

TEST_CASE("jsonb builder: array of strings") {
    auto bytes = jsonb_build([](JsonBuilder& b) {
        b.begin_array("files");
        b.add_element(string_view("data.qi"));
        b.add_element(string_view("settings.db"));
        b.end_array();
    });

    size_t i = 0;
    CHECK(bytes[i++] == jsonb::kBeginObject);

    // begin_array("files")
    CHECK(bytes[i++] == jsonb::kItem);
    i += 6;  // "files\0"
    CHECK(bytes[i++] == jsonb::kBeginArray);

    // "data.qi"
    CHECK(bytes[i++] == jsonb::kString);
    CHECK(memcmp(&bytes[i], "data.qi", 7) == 0); i += 7;
    CHECK(bytes[i++] == '\0');

    // "settings.db"
    CHECK(bytes[i++] == jsonb::kString);
    CHECK(memcmp(&bytes[i], "settings.db", 11) == 0); i += 11;
    CHECK(bytes[i++] == '\0');

    CHECK(bytes[i++] == jsonb::kEndArray);
    CHECK(bytes[i++] == jsonb::kEndObject);
    REQUIRE(i == bytes.size());
}

TEST_CASE("jsonb builder: mixed array types") {
    auto bytes = jsonb_build([](JsonBuilder& b) {
        b.begin_array("mix");
        b.add_element(true);
        b.add_element(int32_t{7});
        b.add_element(3.14);
        b.add_element(string_view("text"));
        b.end_array();
    });

    size_t i = 0;
    CHECK(bytes[i++] == jsonb::kBeginObject);
    // Skip kItem + "mix\0"
    i += 1 + 4;
    CHECK(bytes[i++] == jsonb::kBeginArray);

    CHECK(bytes[i++] == jsonb::kTrue);

    CHECK(bytes[i++] == jsonb::kInt32);
    CHECK(bytes[i] == 7); i += 4;

    CHECK(bytes[i++] == jsonb::kDouble);
    double pi = 3.14;
    CHECK(memcmp(&bytes[i], &pi, 8) == 0); i += 8;

    CHECK(bytes[i++] == jsonb::kString);
    CHECK(memcmp(&bytes[i], "text", 4) == 0); i += 4;
    CHECK(bytes[i++] == '\0');

    CHECK(bytes[i++] == jsonb::kEndArray);
    CHECK(bytes[i++] == jsonb::kEndObject);
    REQUIRE(i == bytes.size());
}

TEST_CASE("jsonb builder: to_view is idempotent") {
    ByteCapture cap;
    StreamingJsonbBuilder b(cap);
    b.add("x", int32_t{1});
    b.to_view();
    size_t first_size = cap.bytes.size();
    b.to_view();  // second call should not emit again
    REQUIRE(cap.bytes.size() == first_size);
}

TEST_CASE("jsonb builder: open builder (streaming transport path)") {
    // In the streaming transport, to_view() is NOT called.
    // The transport writes kEndObject manually.
    auto bytes = jsonb_build_open([](JsonBuilder& b) {
        b.add("req", string_view("card.version"));
    });

    // Should NOT have kEndObject at the end
    CHECK(bytes.back() == '\0');  // last byte is the null terminator of "card.version"

    // First byte is kBeginObject
    CHECK(bytes.front() == jsonb::kBeginObject);
}

TEST_CASE("jsonb builder: add_raw is a no-op") {
    auto bytes = jsonb_build([](JsonBuilder& b) {
        b.add_raw("body", R"({"nested":true})");
    });

    // Only kBeginObject + kEndObject — raw fragment is silently dropped
    REQUIRE(bytes.size() == 2);
    CHECK(bytes[0] == jsonb::kBeginObject);
    CHECK(bytes[1] == jsonb::kEndObject);
}

TEST_CASE("jsonb builder: reset re-emits kBeginObject") {
    ByteCapture cap;
    StreamingJsonbBuilder b(cap);
    b.add("x", int32_t{1});
    b.to_view();
    size_t first_size = cap.bytes.size();

    b.reset();
    // After reset, a new kBeginObject should be emitted
    REQUIRE(cap.bytes.size() == first_size + 1);
    CHECK(cap.bytes.back() == jsonb::kBeginObject);
}

TEST_CASE("jsonb builder: const char* routes to string_view overload") {
    auto bytes = jsonb_build([](JsonBuilder& b) {
        b.add("key", "value");
    });

    // Should contain kString opcode, not kTrue/kFalse (bool overload)
    bool found_string = false;
    for (auto byte : bytes) {
        if (byte == jsonb::kString) { found_string = true; break; }
    }
    CHECK(found_string);
}

// ---------------------------------------------------------------------------
// Wire-compatible with note-c-zero
// ---------------------------------------------------------------------------

TEST_CASE("jsonb builder: card.version matches note-c-zero encoding") {
    // Build {"req":"card.version"} and verify the raw opcode stream
    // matches what note-c-zero's jsonbObjectBegin/jsonbAddStringToObject/
    // jsonbObjectEnd produces (before COBS encoding).
    auto bytes = jsonb_build([](JsonBuilder& b) {
        b.add("req", string_view("card.version"));
    });

    // Expected raw opcodes (pre-COBS):
    //   0x10                       — kBeginObject
    //   0x30 'r' 'e' 'q' 0x00     — kItem "req"
    //   0x40 'c' 'a' 'r' 'd' '.' 'v' 'e' 'r' 's' 'i' 'o' 'n' 0x00 — kString "card.version"
    //   0x11                       — kEndObject
    const uint8_t expected[] = {
        0x10,
        0x30, 'r', 'e', 'q', 0x00,
        0x40, 'c', 'a', 'r', 'd', '.', 'v', 'e', 'r', 's', 'i', 'o', 'n', 0x00,
        0x11,
    };

    REQUIRE(bytes.size() == sizeof(expected));
    CHECK(memcmp(bytes.data(), expected, sizeof(expected)) == 0);
}

// ---------------------------------------------------------------------------
// JSONB Parser — round-trip tests (build → parse → verify events)
// ---------------------------------------------------------------------------

namespace {

// Recording sink that logs SAX events for verification.
struct RecordingSink : JsonSink {
    enum Type { Null, Bool, Int, Float, String, ObjBegin, ObjEnd, ArrBegin, ArrEnd };
    struct Event {
        Type type;
        std::string key;
        bool b = false;
        json_int_t i = 0;
        double f = 0;
        std::string s;
    };
    std::vector<Event> events;

    void on_null(string_view k) override {
        events.push_back({Null, s(k), false, 0, 0, {}});
    }
    void on_bool(string_view k, bool v) override {
        events.push_back({Bool, s(k), v, 0, 0, {}});
    }
    void on_int(string_view k, json_int_t v) override {
        events.push_back({Int, s(k), false, v, 0, {}});
    }
    void on_float(string_view k, double v) override {
        events.push_back({Float, s(k), false, 0, v, {}});
    }
    void on_string(string_view k, string_view v) override {
        events.push_back({String, s(k), false, 0, 0, s(v)});
    }
    void on_object_begin(string_view k) override {
        events.push_back({ObjBegin, s(k), false, 0, 0, {}});
    }
    void on_object_end(string_view k) override {
        events.push_back({ObjEnd, s(k), false, 0, 0, {}});
    }
    void on_array_begin(string_view k) override {
        events.push_back({ArrBegin, s(k), false, 0, 0, {}});
    }
    void on_array_end(string_view k) override {
        events.push_back({ArrEnd, s(k), false, 0, 0, {}});
    }
    static std::string s(string_view sv) {
        return std::string(sv.data(), sv.size());
    }
};

// Read function that serves bytes from a vector.
struct VectorReader {
    const std::vector<uint8_t>& data;
    size_t pos = 0;
    size_t chunk_size = 64;

    Result<size_t> operator()(uint8_t* buf, size_t max, uint32_t) {
        if (pos >= data.size()) return size_t(0);
        size_t n = std::min({max, chunk_size, data.size() - pos});
        memcpy(buf, data.data() + pos, n);
        pos += n;
        return n;
    }
};

// Helper: build JSONB, then parse and return the recorded events.
template<typename BuildFn>
std::vector<RecordingSink::Event> jsonb_round_trip(BuildFn fn) {
    auto opcodes = jsonb_build(fn);
    RecordingSink sink;
    VectorReader reader{opcodes};
    char storage[384];
    SaxStreamBuf buf(storage);
    auto dispatch = make_sax_dispatch(sink);
    auto err = jsonb_parse_streaming(reader, 1000, buf, dispatch);
    REQUIRE(err.empty());
    return sink.events;
}

}  // anonymous namespace

TEST_CASE("jsonb parser: empty object") {
    auto events = jsonb_round_trip([](JsonBuilder&) {});
    REQUIRE(events.size() == 2);
    CHECK(events[0].type == RecordingSink::ObjBegin);
    CHECK(events[0].key.empty());
    CHECK(events[1].type == RecordingSink::ObjEnd);
}

TEST_CASE("jsonb parser: single string field") {
    auto events = jsonb_round_trip([](JsonBuilder& b) {
        b.add("req", string_view("card.version"));
    });
    // object_begin, string("req","card.version"), object_end
    REQUIRE(events.size() == 3);
    CHECK(events[0].type == RecordingSink::ObjBegin);
    CHECK(events[1].type == RecordingSink::String);
    CHECK(events[1].key == "req");
    CHECK(events[1].s == "card.version");
    CHECK(events[2].type == RecordingSink::ObjEnd);
}

TEST_CASE("jsonb parser: int32 field") {
    auto events = jsonb_round_trip([](JsonBuilder& b) {
        b.add("count", int32_t{42});
    });
    REQUIRE(events.size() == 3);
    CHECK(events[1].type == RecordingSink::Int);
    CHECK(events[1].key == "count");
    CHECK(events[1].i == 42);
}

TEST_CASE("jsonb parser: negative int32") {
    auto events = jsonb_round_trip([](JsonBuilder& b) {
        b.add("val", int32_t{-100});
    });
    REQUIRE(events.size() == 3);
    CHECK(events[1].type == RecordingSink::Int);
    CHECK(events[1].i == -100);
}

TEST_CASE("jsonb parser: double field") {
    auto events = jsonb_round_trip([](JsonBuilder& b) {
        b.add("temp", 22.5);
    });
    REQUIRE(events.size() == 3);
    CHECK(events[1].type == RecordingSink::Float);
    CHECK(events[1].key == "temp");
    CHECK(events[1].f == doctest::Approx(22.5));
}

TEST_CASE("jsonb parser: bool fields") {
    auto events = jsonb_round_trip([](JsonBuilder& b) {
        b.add("on", true);
        b.add("off", false);
    });
    REQUIRE(events.size() == 4);
    CHECK(events[1].type == RecordingSink::Bool);
    CHECK(events[1].key == "on");
    CHECK(events[1].b == true);
    CHECK(events[2].type == RecordingSink::Bool);
    CHECK(events[2].key == "off");
    CHECK(events[2].b == false);
}

TEST_CASE("jsonb parser: nested object") {
    auto events = jsonb_round_trip([](JsonBuilder& b) {
        b.add("req", string_view("note.add"));
        b.begin_object("body");
        b.add("temp", 22.5);
        b.add("label", string_view("room"));
        b.end_object();
    });
    // object_begin(""), string("req"), object_begin("body"),
    // float("temp"), string("label"), object_end("body"), object_end("")
    REQUIRE(events.size() == 7);
    CHECK(events[0].type == RecordingSink::ObjBegin);
    CHECK(events[0].key.empty());
    CHECK(events[1].type == RecordingSink::String);
    CHECK(events[1].key == "req");
    CHECK(events[2].type == RecordingSink::ObjBegin);
    CHECK(events[2].key == "body");
    CHECK(events[3].type == RecordingSink::Float);
    CHECK(events[3].key == "temp");
    CHECK(events[4].type == RecordingSink::String);
    CHECK(events[4].key == "label");
    CHECK(events[4].s == "room");
    CHECK(events[5].type == RecordingSink::ObjEnd);
    CHECK(events[5].key == "body");
    CHECK(events[6].type == RecordingSink::ObjEnd);
    CHECK(events[6].key.empty());
}

TEST_CASE("jsonb parser: array of strings") {
    auto events = jsonb_round_trip([](JsonBuilder& b) {
        b.begin_array("files");
        b.add_element(string_view("data.qi"));
        b.add_element(string_view("settings.db"));
        b.end_array();
    });
    // object_begin, array_begin("files"), string("files","data.qi"),
    // string("files","settings.db"), array_end("files"), object_end
    REQUIRE(events.size() == 6);
    CHECK(events[1].type == RecordingSink::ArrBegin);
    CHECK(events[1].key == "files");
    CHECK(events[2].type == RecordingSink::String);
    CHECK(events[2].key == "files");
    CHECK(events[2].s == "data.qi");
    CHECK(events[3].type == RecordingSink::String);
    CHECK(events[3].key == "files");
    CHECK(events[3].s == "settings.db");
    CHECK(events[4].type == RecordingSink::ArrEnd);
    CHECK(events[4].key == "files");
}

TEST_CASE("jsonb parser: null field") {
    // Build manually since JsonBuilder doesn't have add(key, null)
    std::vector<uint8_t> opcodes = {
        jsonb::kBeginObject,
        jsonb::kItem, 'x', '\0',
        jsonb::kNull,
        jsonb::kEndObject,
    };
    RecordingSink sink;
    VectorReader reader{opcodes};
    char storage[384];
    SaxStreamBuf buf(storage);
    auto dispatch = make_sax_dispatch(sink);
    auto err = jsonb_parse_streaming(reader, 1000, buf, dispatch);
    REQUIRE(err.empty());
    REQUIRE(sink.events.size() == 3);
    CHECK(sink.events[1].type == RecordingSink::Null);
    CHECK(sink.events[1].key == "x");
}

TEST_CASE("jsonb parser: small chunk reads") {
    // Ensure the parser handles byte-at-a-time reads.
    auto opcodes = jsonb_build([](JsonBuilder& b) {
        b.add("req", string_view("card.version"));
        b.add("count", int32_t{5});
    });
    RecordingSink sink;
    VectorReader reader{opcodes, 0, 1};  // 1 byte at a time
    char storage[384];
    SaxStreamBuf buf(storage);
    auto dispatch = make_sax_dispatch(sink);
    auto err = jsonb_parse_streaming(reader, 1000, buf, dispatch);
    REQUIRE(err.empty());
    REQUIRE(sink.events.size() == 4);
    CHECK(sink.events[1].key == "req");
    CHECK(sink.events[1].s == "card.version");
    CHECK(sink.events[2].key == "count");
    CHECK(sink.events[2].i == 5);
}

// ---------------------------------------------------------------------------
// CobsStreamWriter
// ---------------------------------------------------------------------------

TEST_CASE("jsonb CobsStreamWriter: round-trip with CobsDecoder") {
    // Write JSONB opcodes through CobsStreamWriter, decode with CobsDecoder,
    // verify the decoded output matches the original opcodes.
    char enc_buf[256];
    JsonBufferWriter enc_writer(enc_buf, sizeof(enc_buf));
    CobsStreamWriter cobs(enc_writer, jsonb::kCobsXor);

    // Write raw JSONB opcode stream for {"req":"card.version"}
    const uint8_t opcodes[] = {
        jsonb::kBeginObject,
        jsonb::kItem, 'r', 'e', 'q', 0x00,
        jsonb::kString, 'c', 'a', 'r', 'd', '.', 'v', 'e', 'r', 's', 'i', 'o', 'n', 0x00,
        jsonb::kEndObject,
    };
    cobs.write(reinterpret_cast<const char*>(opcodes), sizeof(opcodes));
    cobs.flush();

    // Decode
    CobsDecoder decoder(jsonb::kCobsXor);
    std::vector<uint8_t> decoded;
    decoder.feed(reinterpret_cast<const uint8_t*>(enc_buf), enc_writer.pos(),
        [&](const uint8_t* data, size_t n) {
            decoded.insert(decoded.end(), data, data + n);
        });
    decoder.flush([&](const uint8_t* data, size_t n) {
        decoded.insert(decoded.end(), data, data + n);
    });

    REQUIRE(decoded.size() == sizeof(opcodes));
    CHECK(memcmp(decoded.data(), opcodes, sizeof(opcodes)) == 0);
}

TEST_CASE("jsonb CobsStreamWriter: matches CobsEncoder output") {
    // Verify that CobsStreamWriter (byte-at-a-time) produces the same
    // encoded output as CobsEncoder (batch).
    const uint8_t opcodes[] = {
        jsonb::kBeginObject,
        jsonb::kItem, 'k', 0x00,
        jsonb::kInt32, 42, 0, 0, 0,
        jsonb::kEndObject,
    };

    // Batch encode
    std::vector<uint8_t> batch_encoded;
    CobsEncoder encoder(jsonb::kCobsXor);
    encoder.encode(opcodes, sizeof(opcodes),
        [&](const uint8_t* block, size_t n) {
            batch_encoded.insert(batch_encoded.end(), block, block + n);
        });

    // Stream encode
    char stream_buf[256];
    JsonBufferWriter stream_writer(stream_buf, sizeof(stream_buf));
    CobsStreamWriter cobs(stream_writer, jsonb::kCobsXor);
    cobs.write(reinterpret_cast<const char*>(opcodes), sizeof(opcodes));
    cobs.flush();

    REQUIRE(stream_writer.pos() == batch_encoded.size());
    CHECK(memcmp(stream_buf, batch_encoded.data(), batch_encoded.size()) == 0);
}

// ---------------------------------------------------------------------------
// Full JSONB wire framing: {: <COBS> :}\n round-trip
// ---------------------------------------------------------------------------

TEST_CASE("jsonb framing: build framed request, decode and parse") {
    // Build a JSONB request with full wire framing: {: <COBS opcodes> :}\n
    char wire[256];
    JsonBufferWriter wire_writer(wire, sizeof(wire));

    // Write {: header
    wire_writer.write("{:", 2);

    // COBS-encode JSONB opcodes
    CobsStreamWriter cobs(wire_writer, jsonb::kCobsXor);
    StreamingJsonbBuilder builder(cobs);
    builder.add("req", string_view("card.version"));
    cobs.write(reinterpret_cast<const char*>(&jsonb::kEndObject), 1);
    cobs.flush();

    // Write :}\n trailer
    wire_writer.write(":}\n", 3);

    auto framed = wire_writer.view();
    REQUIRE(framed.size() > 5);
    CHECK(framed[0] == '{');
    CHECK(framed[1] == ':');
    CHECK(framed[framed.size() - 3] == ':');
    CHECK(framed[framed.size() - 2] == '}');
    CHECK(framed[framed.size() - 1] == '\n');

    // Now parse: strip framing, COBS-decode, parse JSONB
    // Strip {: header and :}\n trailer
    const uint8_t* payload = reinterpret_cast<const uint8_t*>(framed.data()) + 2;
    size_t payload_len = framed.size() - 5;  // minus "{:" and ":}\n"

    // COBS decode
    std::vector<uint8_t> decoded;
    CobsDecoder decoder(jsonb::kCobsXor);
    decoder.feed(payload, payload_len,
        [&](const uint8_t* data, size_t n) {
            decoded.insert(decoded.end(), data, data + n);
        });
    decoder.flush([&](const uint8_t* data, size_t n) {
        decoded.insert(decoded.end(), data, data + n);
    });

    // Parse JSONB opcodes
    RecordingSink sink;
    VectorReader reader{decoded};
    char storage[384];
    SaxStreamBuf buf(storage);
    auto dispatch = make_sax_dispatch(sink);
    auto err = jsonb_parse_streaming(reader, 1000, buf, dispatch);
    REQUIRE(err.empty());

    // Verify events
    REQUIRE(sink.events.size() == 3);
    CHECK(sink.events[0].type == RecordingSink::ObjBegin);
    CHECK(sink.events[1].type == RecordingSink::String);
    CHECK(sink.events[1].key == "req");
    CHECK(sink.events[1].s == "card.version");
    CHECK(sink.events[2].type == RecordingSink::ObjEnd);
}

// ---------------------------------------------------------------------------
// CobsDecodingReader round-trip
// ---------------------------------------------------------------------------

TEST_CASE("jsonb CobsDecodingReader: decode and parse through reader adapter") {
    // Build COBS-encoded JSONB payload + :} trailer (as it appears on the wire
    // after the {: header has been stripped).
    std::vector<uint8_t> wire_payload;

    // COBS-encode opcodes
    const uint8_t opcodes[] = {
        jsonb::kBeginObject,
        jsonb::kItem, 'v', 'a', 'l', 0x00,
        jsonb::kInt32, 0x2A, 0x00, 0x00, 0x00,  // 42
        jsonb::kEndObject,
    };
    CobsEncoder encoder(jsonb::kCobsXor);
    encoder.encode(opcodes, sizeof(opcodes),
        [&](const uint8_t* block, size_t n) {
            wire_payload.insert(wire_payload.end(), block, block + n);
        });
    // Append :} trailer
    wire_payload.push_back(':');
    wire_payload.push_back('}');

    // Create a reader over the wire payload
    VectorReader wire_reader{wire_payload, 0, 8};  // small chunks

    // Parse through CobsDecodingReader
    detail::CobsDecodingReader<VectorReader> cobs_reader(wire_reader, 1000);
    RecordingSink sink;
    char storage[384];
    SaxStreamBuf buf(storage);
    auto dispatch = make_sax_dispatch(sink);
    auto err = jsonb_parse_streaming(cobs_reader, 1000, buf, dispatch);
    REQUIRE(err.empty());

    REQUIRE(sink.events.size() == 3);
    CHECK(sink.events[0].type == RecordingSink::ObjBegin);
    CHECK(sink.events[1].type == RecordingSink::Int);
    CHECK(sink.events[1].key == "val");
    CHECK(sink.events[1].i == 42);
    CHECK(sink.events[2].type == RecordingSink::ObjEnd);
}

TEST_CASE("jsonb CobsDecodingReader: serial \\r\\n trailer") {
    // Serial protocol sends :}\r\n — verify \r is handled.
    std::vector<uint8_t> wire_payload;

    const uint8_t opcodes[] = {
        jsonb::kBeginObject,
        jsonb::kItem, 'v', 'e', 'r', 's', 'i', 'o', 'n', 0x00,
        jsonb::kString, 'm', 'o', 'c', 'k', 0x00,
        jsonb::kEndObject,
    };
    CobsEncoder encoder(jsonb::kCobsXor);
    encoder.encode(opcodes, sizeof(opcodes),
        [&](const uint8_t* block, size_t n) {
            wire_payload.insert(wire_payload.end(), block, block + n);
        });
    wire_payload.push_back(':');
    wire_payload.push_back('}');
    wire_payload.push_back('\r');  // serial protocol \r before \n

    VectorReader wire_reader{wire_payload, 0, 64};
    detail::CobsDecodingReader<VectorReader> cobs_reader(wire_reader, 1000);
    RecordingSink sink;
    char storage[128];
    SaxStreamBuf buf(storage);
    auto dispatch = make_sax_dispatch(sink);
    auto err = jsonb_parse_streaming(cobs_reader, 1000, buf, dispatch);
    REQUIRE(err.empty());

    REQUIRE(sink.events.size() == 3);
    CHECK(sink.events[0].type == RecordingSink::ObjBegin);
    CHECK(sink.events[1].type == RecordingSink::String);
    CHECK(sink.events[1].key == "version");
    CHECK(sink.events[1].s == "mock");
    CHECK(sink.events[2].type == RecordingSink::ObjEnd);
}

TEST_CASE("jsonb CobsDecodingReader: card.version mock response") {
    // Simulate the exact mock chip card.version JSONB response
    // to verify parsing works with small SaxStreamBuf (128 bytes).
    std::vector<uint8_t> wire_payload;

    uint8_t opcodes[256];
    size_t pos = 0;
    opcodes[pos++] = jsonb::kBeginObject;
    opcodes[pos++] = jsonb::kItem; memcpy(&opcodes[pos], "version\0", 8); pos += 8;
    opcodes[pos++] = jsonb::kString; memcpy(&opcodes[pos], "mock-1.0.0\0", 11); pos += 11;
    opcodes[pos++] = jsonb::kItem; memcpy(&opcodes[pos], "device\0", 7); pos += 7;
    opcodes[pos++] = jsonb::kString; memcpy(&opcodes[pos], "dev:mock\0", 9); pos += 9;
    opcodes[pos++] = jsonb::kItem; memcpy(&opcodes[pos], "board\0", 6); pos += 6;
    opcodes[pos++] = jsonb::kString; memcpy(&opcodes[pos], "1.0\0", 4); pos += 4;
    opcodes[pos++] = jsonb::kEndObject;

    CobsEncoder encoder(jsonb::kCobsXor);
    encoder.encode(opcodes, pos,
        [&](const uint8_t* block, size_t n) {
            wire_payload.insert(wire_payload.end(), block, block + n);
        });
    wire_payload.push_back(':');
    wire_payload.push_back('}');
    wire_payload.push_back('\r');  // serial \r\n

    VectorReader wire_reader{wire_payload, 0, 64};
    detail::CobsDecodingReader<VectorReader> cobs_reader(wire_reader, 1000);
    RecordingSink sink;
    char storage[128];
    SaxStreamBuf buf(storage);
    auto dispatch = make_sax_dispatch(sink);
    auto err = jsonb_parse_streaming(cobs_reader, 1000, buf, dispatch);
    REQUIRE(err.empty());

    // Should have: ObjBegin, String("version"), String("device"), String("board"), ObjEnd
    REQUIRE(sink.events.size() == 5);
    CHECK(sink.events[1].type == RecordingSink::String);
    CHECK(sink.events[1].key == "version");
    CHECK(sink.events[1].s == "mock-1.0.0");
    CHECK(sink.events[2].type == RecordingSink::String);
    CHECK(sink.events[2].key == "device");
    CHECK(sink.events[2].s == "dev:mock");
    CHECK(sink.events[3].type == RecordingSink::String);
    CHECK(sink.events[3].key == "board");
    CHECK(sink.events[3].s == "1.0");
}

TEST_CASE("jsonb parser: truncated stream returns error") {
    // Stream ends before kEndObject — should return an error.
    std::vector<uint8_t> opcodes = {
        jsonb::kBeginObject,
        jsonb::kItem, 'x', '\0',
        jsonb::kString, 'v', '\0',
        // missing kEndObject
    };
    RecordingSink sink;
    VectorReader reader{opcodes};
    char storage[384];
    SaxStreamBuf buf(storage);
    auto dispatch = make_sax_dispatch(sink);
    auto err = jsonb_parse_streaming(reader, 1000, buf, dispatch);
    CHECK_FALSE(err.empty());  // should report truncation
}

TEST_CASE("jsonb parser: empty stream is not an error") {
    std::vector<uint8_t> opcodes = {};
    RecordingSink sink;
    VectorReader reader{opcodes};
    char storage[384];
    SaxStreamBuf buf(storage);
    auto dispatch = make_sax_dispatch(sink);
    auto err = jsonb_parse_streaming(reader, 1000, buf, dispatch);
    CHECK(err.empty());  // no data, no error
    CHECK(sink.events.empty());
}

// Deferred: split-trailer test (`:` in chunk 1, `}` in chunk 2) would need
// pending-byte state machine in CobsDecodingReader. Not a practical concern —
// the real transport returns the full response in one frame_read chunk.

// ---------------------------------------------------------------------------
// Targeted opcode coverage — the StreamingJsonbBuilder only emits
// kInt32 / kDouble / kTrue / kFalse / kString, but the parser must accept
// any width that note-c-zero (or a third-party JSONB producer) emits.
// These tests hand-craft byte streams for the non-builder-emitted opcodes
// and verify the parser produces the right SAX events. Without them the
// kInt8/16/64, kUint8/16/32/64, and kFloat branches in the dispatch
// switch never execute.
// ---------------------------------------------------------------------------

namespace {

// Parse a hand-crafted opcode stream and return the recorded events.
std::vector<RecordingSink::Event> parse_opcodes(const std::vector<uint8_t>& opcodes) {
    RecordingSink sink;
    VectorReader reader{opcodes};
    char storage[384];
    SaxStreamBuf buf(storage);
    auto dispatch = make_sax_dispatch(sink);
    auto err = jsonb_parse_streaming(reader, 1000, buf, dispatch);
    REQUIRE(err.empty());
    return sink.events;
}

// Parse a hand-crafted opcode stream and return any error (or empty string).
std::string parse_opcodes_expect_error(const std::vector<uint8_t>& opcodes) {
    RecordingSink sink;
    VectorReader reader{opcodes};
    char storage[384];
    SaxStreamBuf buf(storage);
    auto dispatch = make_sax_dispatch(sink);
    auto err = jsonb_parse_streaming(reader, 1000, buf, dispatch);
    return std::string(err.data(), err.size());
}

}  // namespace

TEST_CASE("jsonb parser: integer width opcodes dispatch as Int events") {
    // One of each integer opcode in a single object. All dispatch as int32
    // (the SAX layer only carries int32), so we verify the value after the
    // width-specific decode.
    std::vector<uint8_t> opcodes = {
        jsonb::kBeginObject,

        jsonb::kItem, 'a', '\0',
        jsonb::kInt8, 0x80,                          // -128 as signed int8

        jsonb::kItem, 'b', '\0',
        jsonb::kInt16, 0x00, 0x80,                   // -32768 as signed int16 LE

        jsonb::kItem, 'c', '\0',
        jsonb::kInt64,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,  // -1 (truncated to int32)

        jsonb::kItem, 'd', '\0',
        jsonb::kUint8, 0xff,                         // 255

        jsonb::kItem, 'e', '\0',
        jsonb::kUint16, 0x34, 0x12,                  // 0x1234 = 4660

        jsonb::kItem, 'f', '\0',
        jsonb::kUint32, 0x78, 0x56, 0x34, 0x12,      // 0x12345678 = 305419896

        jsonb::kItem, 'g', '\0',
        jsonb::kUint64,
        0x78, 0x56, 0x34, 0x12, 0, 0, 0, 0,          // 0x12345678 truncated

        jsonb::kEndObject,
    };
    auto events = parse_opcodes(opcodes);
    // ObjBegin + 7 ints + ObjEnd
    REQUIRE(events.size() == 9);
    CHECK(events[1].type == RecordingSink::Int);
    CHECK(events[1].key == "a"); CHECK(events[1].i == -128);
    CHECK(events[2].i == -32768);
    CHECK(events[3].i == -1);
    CHECK(events[4].i == 255);
    CHECK(events[5].i == 4660);
    CHECK(events[6].i == 0x12345678);
    CHECK(events[7].i == 0x12345678);
}

TEST_CASE("jsonb parser: kFloat opcode dispatches as Float event") {
    const float f = 3.14f;
    std::vector<uint8_t> opcodes = {
        jsonb::kBeginObject,
        jsonb::kItem, 'v', '\0',
        jsonb::kFloat,
    };
    uint8_t raw[4];
    memcpy(raw, &f, 4);
    opcodes.insert(opcodes.end(), raw, raw + 4);
    opcodes.push_back(jsonb::kEndObject);
    auto events = parse_opcodes(opcodes);
    REQUIRE(events.size() == 3);
    CHECK(events[1].type == RecordingSink::Float);
    CHECK(events[1].key == "v");
    CHECK(events[1].f == doctest::Approx(static_cast<double>(f)));
}

TEST_CASE("jsonb parser: truncated width-prefixed opcodes return errors") {
    // The goal is branch coverage on each NOTE_ERR("truncated X") site —
    // we don't assert on the specific message because NOTE_SHORT_ERRORS=1
    // (enabled by NOTE_MINIMAL) collapses every error literal to "E".
    // Non-empty err is sufficient.
    auto truncate_after = [](uint8_t opcode) {
        return std::vector<uint8_t>{
            jsonb::kBeginObject,
            jsonb::kItem, 'x', '\0',
            opcode,
            // payload missing entirely
        };
    };

    SUBCASE("truncated int8")   { CHECK_FALSE(parse_opcodes_expect_error(truncate_after(jsonb::kInt8)).empty()); }
    SUBCASE("truncated int16")  { CHECK_FALSE(parse_opcodes_expect_error(truncate_after(jsonb::kInt16)).empty()); }
    SUBCASE("truncated int32")  { CHECK_FALSE(parse_opcodes_expect_error(truncate_after(jsonb::kInt32)).empty()); }
    SUBCASE("truncated int64")  { CHECK_FALSE(parse_opcodes_expect_error(truncate_after(jsonb::kInt64)).empty()); }
    SUBCASE("truncated uint8")  { CHECK_FALSE(parse_opcodes_expect_error(truncate_after(jsonb::kUint8)).empty()); }
    SUBCASE("truncated uint16") { CHECK_FALSE(parse_opcodes_expect_error(truncate_after(jsonb::kUint16)).empty()); }
    SUBCASE("truncated uint32") { CHECK_FALSE(parse_opcodes_expect_error(truncate_after(jsonb::kUint32)).empty()); }
    SUBCASE("truncated uint64") { CHECK_FALSE(parse_opcodes_expect_error(truncate_after(jsonb::kUint64)).empty()); }
    SUBCASE("truncated float")  { CHECK_FALSE(parse_opcodes_expect_error(truncate_after(jsonb::kFloat)).empty()); }
    SUBCASE("truncated double") { CHECK_FALSE(parse_opcodes_expect_error(truncate_after(jsonb::kDouble)).empty()); }

    SUBCASE("unknown opcode") {
        std::vector<uint8_t> opcodes = {
            jsonb::kBeginObject,
            jsonb::kItem, 'x', '\0',
            0xEE,  // not a known opcode
            jsonb::kEndObject,
        };
        CHECK_FALSE(parse_opcodes_expect_error(opcodes).empty());
    }
}

TEST_CASE("jsonb builder: add_element(false) emits kFalse opcode") {
    // mixed-array test covers add_element(true); cover the false branch
    // here so both sides of the kTrue/kFalse ternary in JsonBuilder::add_element
    // are exercised.
    auto bytes = jsonb_build([](JsonBuilder& b) {
        b.begin_array("flags");
        b.add_element(false);
        b.add_element(true);
        b.end_array();
    });
    // kBeginObject + kItem + "flags\0" + kBeginArray + kFalse + kTrue +
    // kEndArray + kEndObject = 1 + 1 + 6 + 1 + 1 + 1 + 1 + 1 = 13
    REQUIRE(bytes.size() == 13);
    CHECK(bytes[8]  == jsonb::kBeginArray);
    CHECK(bytes[9]  == jsonb::kFalse);
    CHECK(bytes[10] == jsonb::kTrue);
}

TEST_CASE("jsonb CobsStreamWriter: inner-writer failure propagates from flush") {
    // A JsonWriter that returns false on the first write call. Forces the
    // `!flush_block()` branches in CobsStreamWriter::write to fire.
    struct FailingWriter : JsonWriter {
        bool write(const char*, size_t) override { return false; }
    };

    SUBCASE("flush triggered by a zero byte") {
        FailingWriter inner;
        CobsStreamWriter w(inner, /*xor=*/0);
        const char data[] = {'\0'};
        CHECK_FALSE(w.write(data, 1));
    }

    SUBCASE("flush triggered by 254-byte block fill") {
        FailingWriter inner;
        CobsStreamWriter w(inner, /*xor=*/0);
        // 254 non-zero bytes fills the block to the point where the next
        // byte (or, depending on the encoder, the 255th itself) triggers
        // an auto-flush.
        std::vector<char> filler(254, 'a');
        // First write may succeed or fail depending on when the encoder
        // flushes; the contract under test is just that an inner-writer
        // failure surfaces at some point. Drive enough bytes to force at
        // least one flush attempt.
        bool any_fail = !w.write(filler.data(), filler.size())
                     || !w.write(filler.data(), filler.size());
        CHECK(any_fail);
    }

    SUBCASE("explicit flush() returns false") {
        FailingWriter inner;
        CobsStreamWriter w(inner, /*xor=*/0);
        // Even with no data written, flush emits the (default) code byte.
        CHECK_FALSE(w.flush());
    }
}

TEST_CASE("jsonb parser: deeply nested object beyond kMaxDepth") {
    // The parser maintains an 8-slot key stack (jsonb.hpp kMaxDepth = 8).
    // Going past it must not push or crash — push_key silently stops
    // saving, and pop_key restores `key_len_ = 0` for the over-deep level.
    std::vector<uint8_t> opcodes = {jsonb::kBeginObject};
    // Open 12 nested objects (4 deeper than the stack), each under a key.
    for (int i = 0; i < 12; ++i) {
        opcodes.push_back(jsonb::kItem);
        opcodes.push_back('k');
        opcodes.push_back('\0');
        opcodes.push_back(jsonb::kBeginObject);
    }
    // Close them all.
    for (int i = 0; i < 12; ++i) opcodes.push_back(jsonb::kEndObject);
    opcodes.push_back(jsonb::kEndObject);

    auto events = parse_opcodes(opcodes);
    // 13 ObjBegin + 13 ObjEnd
    REQUIRE(events.size() == 26);
    CHECK(events[0].type == RecordingSink::ObjBegin);
    CHECK(events[25].type == RecordingSink::ObjEnd);
}

// ---------------------------------------------------------------------------
// End-to-end: StaticNotecard + Api + card.version over JSONB mock HAL
// ---------------------------------------------------------------------------

#if NOTE_JSONB
#include <note/static_notecard.hpp>
#include <note/api.hpp>
#include <note/api/card_version.hpp>
#include <note/protocol.hpp>
#include <note/transport_hal.hpp>

namespace {

// Mock Hal that speaks JSONB: accepts a request, responds with
// a canned JSONB card.version response.
struct JsonbMockHal : note::Hal {
    // Canned JSONB response built at construction.
    std::vector<uint8_t> response;
    size_t read_pos = 0;

    JsonbMockHal() {
        // Build card.version JSONB response: {version, device, board}
        uint8_t opcodes[128];
        size_t pos = 0;
        opcodes[pos++] = note::jsonb::kBeginObject;
        opcodes[pos++] = note::jsonb::kItem; memcpy(&opcodes[pos], "version\0", 8); pos += 8;
        opcodes[pos++] = note::jsonb::kString; memcpy(&opcodes[pos], "mock-1.0.0\0", 11); pos += 11;
        opcodes[pos++] = note::jsonb::kItem; memcpy(&opcodes[pos], "device\0", 7); pos += 7;
        opcodes[pos++] = note::jsonb::kString; memcpy(&opcodes[pos], "dev:mock\0", 9); pos += 9;
        opcodes[pos++] = note::jsonb::kItem; memcpy(&opcodes[pos], "board\0", 6); pos += 6;
        opcodes[pos++] = note::jsonb::kString; memcpy(&opcodes[pos], "1.0\0", 4); pos += 4;
        opcodes[pos++] = note::jsonb::kEndObject;

        // Frame: {:<COBS>:}\r\n
        response.push_back('{');
        response.push_back(':');
        note::CobsEncoder encoder(note::jsonb::kCobsXor);
        encoder.encode(opcodes, pos,
            [&](const uint8_t* block, size_t n) {
                response.insert(response.end(), block, block + n);
            });
        response.push_back(':');
        response.push_back('}');
        response.push_back('\r');
        response.push_back('\n');
    }

    bool transmit(const uint8_t*, size_t) override { return true; }

    size_t chunk_size = 1;  // byte at a time, like slow serial

    note::Result<size_t> read(uint8_t* buf, size_t max, uint32_t) override {
        if (read_pos >= response.size()) return size_t(0);
        size_t n = std::min({max, chunk_size, response.size() - read_pos});
        memcpy(buf, response.data() + read_pos, n);
        read_pos += n;
        return n;
    }

    bool reset() override { read_pos = 0; return true; }
    bool write_line_terminator() override {
        read_pos = 0;  // reset for next read
        return true;
    }
    void delay(uint32_t) override {}
    uint32_t ms_ = 0;
    uint32_t millis() override { return ms_++; }
};

}  // anonymous namespace

#if !NOTE_NO_POLYMORPHIC
TEST_CASE("jsonb end-to-end: Notecard card.version") {
    // Replicate the exact AVR path: StaticNotecard + Api + card.version
    // with a JSONB mock HAL.
    using CardVersion = note::api::CardVersion;

    alignas(4) char arena_buf[CardVersion::Response::max_arena_size];
    note::MonotonicArena arena(arena_buf);

    JsonbMockHal hal;
    note::Protocol transport(hal);
    // Use Notecard directly since StaticNotecard is template-heavy
    note::Notecard nc(transport, note::arena_allocator(arena));
    note::Api<> api(nc);

    auto rsp = api.card.version().execute();
    if (!rsp) {
        auto e = rsp.error();
        printf("execute error: code=%d cause=%d\n",
               static_cast<int>(e.code), static_cast<int>(e.cause));
    }
    REQUIRE(rsp.has_value());
    CHECK(rsp.version.has_value());
    CHECK(rsp.device.has_value());
    CHECK(rsp.board.has_value());
    if (rsp.version.has_value())
        CHECK(rsp.version.value() == "mock-1.0.0");
    if (rsp.device.has_value())
        CHECK(rsp.device.value() == "dev:mock");
}
#endif // NOTE_NO_POLYMORPHIC
#endif // NOTE_JSONB
