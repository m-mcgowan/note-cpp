/// @file test_jsonb_i2c.cpp
/// JSONB integration tests over I2C — both raw-encoder and library-based.
///
/// Raw-encoder: sends JSONB using note-c-zero's C library — the reference
/// implementation, used as a baseline for what the Notecard accepts on the
/// wire. Library tests: sends JSONB using note-cpp's StreamingJsonbBuilder
/// + CobsStreamWriter and parses responses with jsonb_parse_streaming.
///
/// Large work buffers (response, wire, COBS-decoded payload, SAX parser
/// storage) live at file scope rather than on the test function's stack, to
/// keep the loop-task stack shallow on ESP32-S3.
///
/// I/O goes through the production HAL (note::arduino::I2cHal) rather than raw
/// Wire calls: its transmit/receive primitives cap each read at the I2C MTU,
/// so a single requestFrom can never exceed the Wire RX buffer. An earlier
/// hand-rolled receive issued requestFrom(addr, 2 + 250) into the 128-byte
/// default buffer, overflowing the heap and corrupting an adjacent TLSF block
/// header — a guru-meditation that only surfaced on a later heap walk. See
/// ISSUE-jsonb-streaming-parse-crash-esp32.md.

#include "../include/hal_i2c.hpp"
#ifdef NOTECARD_TEST_I2C

#include <doctest.h>
#include <cstdio>
#include <cstring>

#include <note/jsonb.hpp>
#include <note/link/cobs.hpp>
#include <note/link/i2c.hpp>
#include <note/json_sax.hpp>
#include <note/json_sax_streaming.hpp>
#include <note/lexer/sax_adapter.hpp>

extern "C" {
#include "jsonb/jsonb.h"
}

// Reuse the shared Wire instance from test_i2c.cpp.
extern TwoWire& notecardWire();

namespace {

// File-scope work buffers — single allocation in BSS, reused across tests.
// Keeping these off the stack prevents doctest's cross-SUBCASE state from
// being clipped when the loop task's stack tightens.
uint8_t s_resp[512];
char    s_wire_buf[256];
uint8_t s_decoded[512];
char    s_storage[512];

// Production-HAL view of the shared Wire bus. These tests deliberately bypass
// the note-cpp request pipeline (they exercise raw JSONB on the wire), but they
// still go through the HAL's transmit/receive primitives rather than touching
// Wire directly. Hand-rolling raw `Wire.requestFrom(addr, 2 + N)` with N up to
// 250 once overflowed the 128-byte Wire RX buffer and corrupted the heap; the
// HAL caps every read at kI2cDefaultMtu (30) + 2 bytes, so the overflow class
// cannot occur. test_i2c.cpp drives binary transfers exactly the same way.
NotecardI2cHal& notecardHal() {
    static NotecardI2cHal hal{notecardWire(), NOTECARD_I2C_SDA, NOTECARD_I2C_SCL};
    return hal;
}

// Send a buffer to the Notecard, chunked at the HAL MTU (SoI2C [size][data]).
bool i2c_send(const uint8_t* buf, size_t len) {
    NotecardI2cHal& hal = notecardHal();
    const size_t mtu = hal.max_transfer();
    size_t offset = 0;
    while (offset < len) {
        size_t chunk = (len - offset < mtu) ? (len - offset) : mtu;
        if (!hal.transmit(buf + offset, chunk)) return false;
        offset += chunk;
    }
    return true;
}

// Receive a newline-terminated response, polling available bytes and reading
// at most one MTU per HAL receive() (which caps requestFrom at MTU + 2).
size_t i2c_recv(uint8_t* buf, size_t buf_size, uint32_t timeout_ms) {
    NotecardI2cHal& hal = notecardHal();
    const size_t mtu = hal.max_transfer();
    size_t received = 0;
    uint32_t avail = 0;
    uint32_t deadline = hal.millis() + timeout_ms;
    bool found_eop = false;
    hal.receive(buf, 0, avail);                       // priming query
    while (hal.millis() < deadline) {
        size_t want = avail;
        if (want > mtu) want = mtu;
        if (received + want > buf_size) want = buf_size - received;
        if (want > 0) {
            if (!hal.receive(buf + received, want, avail)) { hal.delay(10); continue; }
            received += want;
            if (received > 0 && buf[received - 1] == '\n') found_eop = true;
        }
        if (found_eop && avail == 0) break;
        if (avail == 0) { hal.delay(50); hal.receive(buf + received, 0, avail); }
    }
    return received;
}

// Build a framed JSONB `card.version` request into s_wire_buf.
size_t build_jsonb_card_version() {
    note::JsonBufferWriter writer(s_wire_buf, sizeof(s_wire_buf));
    writer.write("{:", 2);
    note::CobsStreamWriter cobs(writer, note::jsonb::kCobsXor);
    note::StreamingJsonbBuilder builder(cobs);
    builder.add("req", note::string_view("card.version"));
    cobs.write(reinterpret_cast<const char*>(&note::jsonb::kEndObject), 1);
    cobs.flush();
    writer.write(":}\n", 3);
    return writer.pos();
}

// COBS-decode the `{:...:}\n`-framed JSONB response in s_resp into s_decoded.
// Returns the decoded byte count, or SIZE_MAX on framing error.
size_t decode_jsonb_resp(size_t resp_len) {
    if (resp_len < 5 || s_resp[0] != '{' || s_resp[1] != ':') return SIZE_MAX;
    size_t payload_len = resp_len;
    while (payload_len > 0 && s_resp[payload_len - 1] == '\n') payload_len--;
    if (payload_len < 4 || s_resp[payload_len - 1] != '}' || s_resp[payload_len - 2] != ':') return SIZE_MAX;
    payload_len -= 4;
    size_t decoded_len = 0;
    note::CobsDecoder decoder(note::jsonb::kCobsXor);
    auto write_cb = [&](const uint8_t* data, size_t n) {
        size_t room = sizeof(s_decoded) - decoded_len;
        size_t copy = (n > room) ? room : n;
        memcpy(s_decoded + decoded_len, data, copy);
        decoded_len += copy;
    };
    decoder.feed(s_resp + 2, payload_len, write_cb);
    decoder.flush(write_cb);
    return decoded_len;
}

// Reader over a byte span for the streaming JSONB parser.
struct SpanReader {
    const uint8_t* data;
    size_t size;
    size_t pos = 0;
    note::Result<size_t> operator()(uint8_t* buf, size_t max, uint32_t) {
        if (pos >= size) return size_t(0);
        size_t n = (max < size - pos) ? max : size - pos;
        memcpy(buf, data + pos, n);
        pos += n;
        return n;
    }
};

// Sink for the `card.version` SUBCASE — copies a few fields into fixed
// buffers (avoiding std::string heap churn).
struct VersionSink : note::JsonSink {
    static constexpr size_t kBufSize = 64;
    char version[kBufSize] = {};
    char device[kBufSize] = {};

    void on_string(note::string_view k, note::string_view v) override {
        if (k == "version") copy_to(version, v);
        else if (k == "device") copy_to(device, v);
    }
    static void copy_to(char* dst, note::string_view v) {
        size_t n = (v.size() < kBufSize - 1) ? v.size() : (kBufSize - 1);
        memcpy(dst, v.data(), n);
        dst[n] = 0;
    }
};

// Probe sink — checks whether the parsed object contains any field
// named "crc" (string/int/float/bool). Field count is reported but
// names aren't retained (this is investigation output, not a regression
// assertion).
struct CrcProbeSink : note::JsonSink {
    bool found_crc = false;
    int field_count = 0;
    void on_string(note::string_view k, note::string_view) override { observe(k); }
    void on_int(note::string_view k, note::json_int_t) override { observe(k); }
    void on_float(note::string_view k, double) override { observe(k); }
    void on_bool(note::string_view k, bool) override { observe(k); }
    void observe(note::string_view k) {
        ++field_count;
        if (k == "crc") found_crc = true;
    }
};

}  // namespace

TEST_CASE("JSONB: card.version over I2C") {
    // First, verify normal JSON works (sanity check)
    SUBCASE("sanity: JSON card.version works") {
        const char* json_req = "{\"req\":\"card.version\"}\n";

        bool sent = i2c_send((const uint8_t*)json_req, strlen(json_req));
        REQUIRE(sent);

        size_t resp_len = i2c_recv(s_resp, sizeof(s_resp), 5000);
        REQUIRE(resp_len > 0);
        s_resp[resp_len < sizeof(s_resp) ? resp_len : sizeof(s_resp) - 1] = '\0';

        printf("  JSON response (%zu bytes): %s\n", resp_len, (char*)s_resp);
        CHECK(strstr((char*)s_resp, "version") != nullptr);
    }

    delay(500);

    // Raw C library baseline (note-c-zero) — the reference encoding.
    SUBCASE("raw: JSONB card.version (note-c-zero)") {
        // Reuse s_decoded as the build buffer for this subcase only.
        jsonbContext ctx;
        jsonbObjectBegin(&ctx, s_decoded, sizeof(s_decoded), nullptr);
        jsonbAddStringToObject(&ctx, "req", "card.version");
        uint32_t len = jsonbObjectEnd(&ctx);
        REQUIRE(len > 0);

        printf("  JSONB request (%u bytes):", len);
        for (uint32_t i = 0; i < len; i++) printf(" %02x", s_decoded[i]);
        printf("\n");

        bool sent = i2c_send(s_decoded, len);
        REQUIRE(sent);

        size_t resp_len = i2c_recv(s_resp, sizeof(s_resp), 5000);

        printf("  Response (%zu bytes):", resp_len);
        if (resp_len > 0) {
            for (size_t i = 0; i < resp_len && i < 64; i++) printf(" %02x", s_resp[i]);
            if (resp_len > 64) printf(" ...");
            printf("\n");

            if (jsonbPresent(s_resp, resp_len)) {
                printf("  Response is JSONB! Parsing...\n");
                jsonbContext rsp_ctx;
                if (jsonbParse(&rsp_ctx, s_resp, resp_len)) {
                    char* version = jsonbGetString(&rsp_ctx, "version");
                    char* device = jsonbGetString(&rsp_ctx, "device");
                    printf("  version: %s\n", version);
                    printf("  device:  %s\n", device);
                    CHECK(strlen(version) > 0);
                }
            }
        }
        CHECK(resp_len > 0);
    }

    delay(500);

    // Library-based test: build with note-cpp, parse response with note-cpp
    SUBCASE("library: JSONB card.version via note-cpp") {
        size_t req_len = build_jsonb_card_version();
        printf("  note-cpp JSONB request (%zu bytes):", req_len);
        for (size_t i = 0; i < req_len; i++) printf(" %02x", (uint8_t)s_wire_buf[i]);
        printf("\n");

        bool sent = i2c_send(reinterpret_cast<const uint8_t*>(s_wire_buf), req_len);
        REQUIRE(sent);

        size_t resp_len = i2c_recv(s_resp, sizeof(s_resp), 5000);
        REQUIRE(resp_len > 2);

        printf("  Response (%zu bytes):", resp_len);
        for (size_t i = 0; i < resp_len && i < 64; i++) printf(" %02x", s_resp[i]);
        if (resp_len > 64) printf(" ...");
        printf("\n");

        size_t decoded_len = decode_jsonb_resp(resp_len);
        REQUIRE(decoded_len != SIZE_MAX);

        printf("  Decoded JSONB (%zu bytes):", decoded_len);
        for (size_t i = 0; i < decoded_len && i < 40; i++) printf(" %02x", s_decoded[i]);
        if (decoded_len > 40) printf(" ...");
        printf("\n");

        VersionSink sink;
        SpanReader reader{s_decoded, decoded_len};
        note::SaxStreamBuf parse_buf(s_storage);
        auto dispatch = note::make_sax_dispatch(sink);
        auto err = note::jsonb_parse_streaming(reader, 1000, parse_buf, dispatch);

        printf("  version: %s\n", sink.version);
        printf("  device:  %s\n", sink.device);

        CHECK(err.empty());
        CHECK(strlen(sink.version) > 0);
        CHECK(strlen(sink.device) > 0);
    }
}

// CRC investigation: when this firmware doesn't include CRC in JSON
// responses, JSONB also won't carry CRC. This documents that finding
// by probing both wire formats and reporting which (if any) carry a
// "crc" field.
TEST_CASE("JSONB CRC investigation: does Notecard include crc in JSONB responses?") {
    // Step 1: probe JSON response.
    const char* json_req = "{\"req\":\"card.version\"}\n";
    REQUIRE(i2c_send((const uint8_t*)json_req, strlen(json_req)));
    size_t json_len = i2c_recv(s_resp, sizeof(s_resp), 5000);
    REQUIRE(json_len > 0);
    s_resp[json_len < sizeof(s_resp) ? json_len : sizeof(s_resp) - 1] = '\0';

    bool json_has_crc = (strstr((char*)s_resp, "\"crc\"") != nullptr);
    printf("  [CRC test] JSON response has crc field: %s\n", json_has_crc ? "YES" : "NO");

    delay(500);

    // Step 2: probe JSONB response.
    size_t req_len = build_jsonb_card_version();
    REQUIRE(i2c_send(reinterpret_cast<const uint8_t*>(s_wire_buf), req_len));
    size_t jsonb_resp_len = i2c_recv(s_resp, sizeof(s_resp), 5000);
    REQUIRE(jsonb_resp_len > 4);

    size_t decoded_len = decode_jsonb_resp(jsonb_resp_len);
    REQUIRE(decoded_len != SIZE_MAX);

    CrcProbeSink sink;
    SpanReader reader{s_decoded, decoded_len};
    note::SaxStreamBuf parse_buf(s_storage);
    auto dispatch = note::make_sax_dispatch(sink);
    auto err = note::jsonb_parse_streaming(reader, 1000, parse_buf, dispatch);

    printf("  [CRC test] JSONB parse error: %s\n", err.empty() ? "(none)" : "ERROR");
    printf("  [CRC test] JSONB response field count: %d\n", sink.field_count);
    printf("  [CRC test] JSONB response %s a 'crc' field\n",
        sink.found_crc ? "DOES contain" : "does NOT contain");

    CHECK(err.empty());
    CHECK(sink.field_count > 0);
}

#endif // NOTECARD_TEST_I2C
