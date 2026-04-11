/// @file test_jsonb_spike.cpp
/// JSONB integration tests over I2C — both raw spike and library-based.
///
/// Raw spike: sends JSONB using note-c-zero's C library.
/// Library tests: sends JSONB using note-cpp's StreamingJsonbBuilder +
/// CobsStreamWriter, parses responses with jsonb_parse_streaming.

#include "../include/hal_i2c.hpp"
#ifdef NOTECARD_TEST_I2C

#include <doctest.h>
#include <cstdio>

#include <note/jsonb.hpp>
#include <note/transport/cobs.hpp>
#include <note/transport/i2c.hpp>
#include <note/json_sax.hpp>
#include <note/json_sax_streaming.hpp>
#include <note/lexer/sax_adapter.hpp>

extern "C" {
#include "jsonb/jsonb.h"
}

// Reuse the shared Wire instance from test_i2c.cpp.
extern TwoWire& notecardWire();

// Send a newline-terminated buffer using the Notecard I2C protocol.
// The protocol: write [length_byte][payload_chunk] in chunks up to 250 bytes,
// with the first byte of each I2C write being the chunk length.
static bool i2c_send(TwoWire& wire, uint8_t addr, const uint8_t* buf, size_t len) {
    constexpr size_t MAX_CHUNK = 250;
    size_t offset = 0;
    while (offset < len) {
        size_t chunk = len - offset;
        if (chunk > MAX_CHUNK) chunk = MAX_CHUNK;
        wire.beginTransmission(addr);
        wire.write(static_cast<uint8_t>(chunk));
        wire.write(buf + offset, chunk);
        if (wire.endTransmission() != 0) return false;
        offset += chunk;
        if (offset < len) delay(1);
    }
    return true;
}

// Receive a newline-terminated response using the Notecard I2C protocol.
static size_t i2c_recv(TwoWire& wire, uint8_t addr, uint8_t* buf, size_t buf_size, uint32_t timeout_ms) {
    constexpr size_t MAX_CHUNK = 250;
    size_t received = 0;
    uint32_t deadline = millis() + timeout_ms;
    size_t read_len = 0;

    while (millis() < deadline) {
        wire.beginTransmission(addr);
        wire.write((uint8_t)0);
        wire.write((uint8_t)read_len);
        wire.endTransmission();

        size_t response_size = 2 + read_len;
        size_t got = wire.requestFrom(addr, response_size);
        if (got < 2) { delay(50); read_len = 0; continue; }

        uint8_t available = wire.read();
        uint8_t returned = wire.read();

        for (uint8_t i = 0; i < returned && received < buf_size; i++) {
            if ((size_t)(i + 2) < got) {
                buf[received++] = wire.read();
            }
        }

        if (received > 0 && buf[received - 1] == '\n') break;

        if (available > 0) {
            read_len = available;
            if (read_len > MAX_CHUNK) read_len = MAX_CHUNK;
        } else {
            read_len = 0;
            delay(50);
        }
    }
    return received;
}

TEST_CASE("JSONB spike: card.version over I2C") {
    constexpr uint8_t NOTECARD_I2C_ADDR = 0x17;
    auto& wire = notecardWire();

    // First, verify normal JSON works (sanity check)
    SUBCASE("sanity: JSON card.version works") {
        const char* json_req = "{\"req\":\"card.version\"}\n";
        uint8_t resp[512] = {};

        bool sent = i2c_send(wire, NOTECARD_I2C_ADDR, (const uint8_t*)json_req, strlen(json_req));
        REQUIRE(sent);

        size_t resp_len = i2c_recv(wire, NOTECARD_I2C_ADDR, resp, sizeof(resp), 5000);
        REQUIRE(resp_len > 0);
        resp[resp_len] = '\0';

        printf("  JSON response (%zu bytes): %s\n", resp_len, (char*)resp);
        CHECK(strstr((char*)resp, "version") != nullptr);
    }

    delay(500);

    // Raw C library spike
    SUBCASE("spike: JSONB card.version (C library)") {
        uint8_t buf[256] = {};
        jsonbContext ctx;

        jsonbObjectBegin(&ctx, buf, sizeof(buf), nullptr);
        jsonbAddStringToObject(&ctx, "req", "card.version");
        uint32_t len = jsonbObjectEnd(&ctx);
        REQUIRE(len > 0);

        printf("  JSONB request (%u bytes): ", len);
        for (uint32_t i = 0; i < len; i++) printf("%02x ", buf[i]);
        printf("\n");

        bool sent = i2c_send(wire, NOTECARD_I2C_ADDR, buf, len);
        REQUIRE(sent);

        uint8_t resp[512] = {};
        size_t resp_len = i2c_recv(wire, NOTECARD_I2C_ADDR, resp, sizeof(resp), 5000);

        printf("  Response (%zu bytes): ", resp_len);
        if (resp_len > 0) {
            for (size_t i = 0; i < resp_len && i < 64; i++) printf("%02x ", resp[i]);
            if (resp_len > 64) printf("...");
            printf("\n");

            if (jsonbPresent(resp, resp_len)) {
                printf("  Response is JSONB! Parsing...\n");
                jsonbContext rsp_ctx;
                if (jsonbParse(&rsp_ctx, resp, resp_len)) {
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
        // 1. Build framed JSONB request: {:<COBS opcodes>:}\n
        char wire_buf[256];
        note::JsonBufferWriter wire_writer(wire_buf, sizeof(wire_buf));
        wire_writer.write("{:", 2);
        note::CobsStreamWriter cobs(wire_writer, note::jsonb::kCobsXor);
        note::StreamingJsonbBuilder builder(cobs);
        builder.add("req", note::string_view("card.version"));
        cobs.write(reinterpret_cast<const char*>(&note::jsonb::kEndObject), 1);
        cobs.flush();
        wire_writer.write(":}\n", 3);

        size_t req_len = wire_writer.pos();
        printf("  note-cpp JSONB request (%zu bytes):", req_len);
        for (size_t i = 0; i < req_len; i++) printf(" %02x", (uint8_t)wire_buf[i]);
        printf("\n");

        // 2. Send via I2C
        bool sent = i2c_send(wire, NOTECARD_I2C_ADDR,
            reinterpret_cast<const uint8_t*>(wire_buf), req_len);
        REQUIRE(sent);

        // 3. Receive response
        uint8_t resp[512] = {};
        size_t resp_len = i2c_recv(wire, NOTECARD_I2C_ADDR, resp, sizeof(resp), 5000);
        REQUIRE(resp_len > 2);

        printf("  Response (%zu bytes):", resp_len);
        for (size_t i = 0; i < resp_len && i < 64; i++) printf(" %02x", resp[i]);
        if (resp_len > 64) printf(" ...");
        printf("\n");

        // 4. Verify JSONB framing
        REQUIRE(resp[0] == '{');
        REQUIRE(resp[1] == ':');

        // 5. Strip {: header and :}\n trailer, COBS-decode
        const uint8_t* payload = resp + 2;
        size_t payload_len = resp_len;
        // Find :}\n trailer
        while (payload_len > 0 && resp[payload_len - 1] == '\n') payload_len--;
        REQUIRE(payload_len >= 4);  // at least {: + :}
        REQUIRE(resp[payload_len - 1] == '}');
        REQUIRE(resp[payload_len - 2] == ':');
        payload_len -= 4;  // strip "{:" prefix and ":}" suffix

        std::vector<uint8_t> decoded;
        note::CobsDecoder decoder(note::jsonb::kCobsXor);
        decoder.feed(payload, payload_len,
            [&](const uint8_t* data, size_t n) {
                decoded.insert(decoded.end(), data, data + n);
            });
        decoder.flush([&](const uint8_t* data, size_t n) {
            decoded.insert(decoded.end(), data, data + n);
        });

        printf("  Decoded JSONB (%zu bytes):", decoded.size());
        for (size_t i = 0; i < decoded.size() && i < 40; i++) printf(" %02x", decoded[i]);
        if (decoded.size() > 40) printf(" ...");
        printf("\n");

        // 6. Parse JSONB with note-cpp parser
        struct VersionSink : note::JsonSink {
            std::string version;
            std::string device;
            std::string body;

            void on_string(note::string_view k, note::string_view v) override {
                if (k == "version") version.assign(v.data(), v.size());
                else if (k == "device") device.assign(v.data(), v.size());
                else if (k == "body") body.assign(v.data(), v.size());
            }
        } sink;

        struct VectorReader {
            const std::vector<uint8_t>& data;
            size_t pos = 0;
            note::Result<size_t> operator()(uint8_t* buf, size_t max, uint32_t) {
                if (pos >= data.size()) return size_t(0);
                size_t n = max < (data.size() - pos) ? max : (data.size() - pos);
                memcpy(buf, data.data() + pos, n);
                pos += n;
                return n;
            }
        } reader{decoded};

        char storage[384];
        note::SaxStreamBuf buf(storage);
        auto dispatch = note::make_sax_dispatch(sink);
        auto err = note::jsonb_parse_streaming(reader, 1000, buf, dispatch);

        printf("  version: %s\n", sink.version.c_str());
        printf("  device:  %s\n", sink.device.c_str());

        CHECK(err.empty());
        CHECK(sink.version.size() > 0);
        CHECK(sink.device.size() > 0);
    }
}

#endif // NOTECARD_TEST_I2C
