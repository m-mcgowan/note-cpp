// Tests for JSONB opcode constants and StreamingJsonbBuilder.

#include "catch.hpp"

#include <note/jsonb.hpp>

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
