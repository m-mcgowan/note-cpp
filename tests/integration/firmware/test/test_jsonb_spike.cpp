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

TEST_CASE("JSONB CRC investigation: does Notecard include crc in JSONB responses?") {
    constexpr uint8_t NOTECARD_I2C_ADDR = 0x17;
    auto& wire = notecardWire();

    // Step 1: Send JSON card.version — check if the Notecard includes a "crc" field.
    // This tells us whether this firmware version supports CRC at all.
    {
        const char* json_req = "{\"req\":\"card.version\"}\n";
        uint8_t resp[512] = {};

        bool sent = i2c_send(wire, NOTECARD_I2C_ADDR, (const uint8_t*)json_req, strlen(json_req));
        REQUIRE(sent);

        size_t resp_len = i2c_recv(wire, NOTECARD_I2C_ADDR, resp, sizeof(resp), 5000);
        REQUIRE(resp_len > 0);
        resp[resp_len] = '\0';

        printf("  [CRC test] JSON response (%zu bytes): %s", resp_len, (char*)resp);

        bool json_has_crc = (strstr((char*)resp, "\"crc\"") != nullptr);
        printf("  [CRC test] JSON response has crc field: %s\n", json_has_crc ? "YES" : "NO");

        if (!json_has_crc) {
            printf("  [CRC test] This firmware does not include CRC in JSON responses.\n");
            printf("  [CRC test] Cannot test JSONB CRC — skipping.\n");
            return;
        }
    }

    delay(500);

    // Step 2: Send JSONB card.version — check if the Notecard includes a "crc" field
    // in the JSONB response.
    {
        // Build framed JSONB request: {:<COBS opcodes>:}\n
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
        bool sent = i2c_send(wire, NOTECARD_I2C_ADDR,
            reinterpret_cast<const uint8_t*>(wire_buf), req_len);
        REQUIRE(sent);

        uint8_t resp[512] = {};
        size_t resp_len = i2c_recv(wire, NOTECARD_I2C_ADDR, resp, sizeof(resp), 5000);
        REQUIRE(resp_len > 4);
        REQUIRE(resp[0] == '{');
        REQUIRE(resp[1] == ':');

        printf("  [CRC test] JSONB response (%zu bytes):", resp_len);
        for (size_t i = 0; i < resp_len; i++) printf(" %02x", resp[i]);
        printf("\n");

        // Strip framing and COBS-decode
        const uint8_t* payload = resp + 2;
        size_t payload_len = resp_len;
        while (payload_len > 0 && resp[payload_len - 1] == '\n') payload_len--;
        REQUIRE(payload_len >= 4);
        REQUIRE(resp[payload_len - 1] == '}');
        REQUIRE(resp[payload_len - 2] == ':');
        payload_len -= 4;

        std::vector<uint8_t> decoded;
        note::CobsDecoder decoder(note::jsonb::kCobsXor);
        decoder.feed(payload, payload_len,
            [&](const uint8_t* data, size_t n) {
                decoded.insert(decoded.end(), data, data + n);
            });
        decoder.flush([&](const uint8_t* data, size_t n) {
            decoded.insert(decoded.end(), data, data + n);
        });

        printf("  [CRC test] Decoded JSONB (%zu bytes):", decoded.size());
        for (size_t i = 0; i < decoded.size(); i++) printf(" %02x", decoded[i]);
        printf("\n");

        // Parse JSONB and capture ALL field names, specifically looking for "crc"
        struct CrcProbeSink : note::JsonSink {
            bool found_crc = false;
            std::string crc_value;
            std::vector<std::string> all_fields;

            void on_string(note::string_view k, note::string_view v) override {
                all_fields.emplace_back(k.data(), k.size());
                if (k == "crc") {
                    found_crc = true;
                    crc_value.assign(v.data(), v.size());
                }
            }
            void on_int(note::string_view k, note::json_int_t) override {
                all_fields.emplace_back(k.data(), k.size());
                if (k == "crc") found_crc = true;
            }
            void on_float(note::string_view k, double) override {
                all_fields.emplace_back(k.data(), k.size());
                if (k == "crc") found_crc = true;
            }
            void on_bool(note::string_view k, bool) override {
                all_fields.emplace_back(k.data(), k.size());
                if (k == "crc") found_crc = true;
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

        char storage[512];
        note::SaxStreamBuf buf(storage);
        auto dispatch = note::make_sax_dispatch(sink);
        auto err = note::jsonb_parse_streaming(reader, 1000, buf, dispatch);

        printf("  [CRC test] Parse error: %s\n", err.empty() ? "(none)" : std::string(err).c_str());
        printf("  [CRC test] Fields in JSONB response:");
        for (auto& f : sink.all_fields) printf(" '%s'", f.c_str());
        printf("\n");

        printf("\n  ============================================\n");
        printf("  RESULT: JSONB response %s a 'crc' field\n",
            sink.found_crc ? "DOES contain" : "does NOT contain");
        if (sink.found_crc) {
            printf("  CRC value: '%s'\n", sink.crc_value.c_str());
            printf("  → note-cpp SHOULD support CRC for JSONB responses\n");
        } else {
            printf("  → CRC is JSON-only; JSONB path can safely skip CRC handling\n");
        }
        printf("  ============================================\n\n");

        CHECK(err.empty());
    }

    delay(500);

    // Step 3: Send JSON request WITH a CRC field (to explicitly enable CRC mode),
    // then send JSONB again to see if CRC persists.
    {
        // First, send a JSON request that includes a crc field.
        // Use seq=0001, crc is CRC32 of the request body (without the crc field).
        // We'll just send an intentionally wrong CRC to see if the Notecard still
        // responds — the Notecard might reject it or ignore it.
        // Actually, let's compute a proper CRC. The CRC covers everything in the
        // request except the crc field itself (i.e., the JSON up to the crc field).
        //
        // Simpler approach: send two JSON requests so the Notecard auto-enables CRC,
        // then send JSONB.
        const char* json_req2 = "{\"req\":\"card.version\"}\n";
        uint8_t resp2[512] = {};
        bool sent2 = i2c_send(wire, NOTECARD_I2C_ADDR, (const uint8_t*)json_req2, strlen(json_req2));
        REQUIRE(sent2);
        size_t resp_len2 = i2c_recv(wire, NOTECARD_I2C_ADDR, resp2, sizeof(resp2), 5000);
        REQUIRE(resp_len2 > 0);
        resp2[resp_len2] = '\0';
        printf("  [CRC test] 2nd JSON response: %s", (char*)resp2);

        delay(500);

        // Now send JSONB again
        char wire_buf3[256];
        note::JsonBufferWriter wire_writer3(wire_buf3, sizeof(wire_buf3));
        wire_writer3.write("{:", 2);
        note::CobsStreamWriter cobs3(wire_writer3, note::jsonb::kCobsXor);
        note::StreamingJsonbBuilder builder3(cobs3);
        builder3.add("req", note::string_view("card.version"));
        cobs3.write(reinterpret_cast<const char*>(&note::jsonb::kEndObject), 1);
        cobs3.flush();
        wire_writer3.write(":}\n", 3);

        size_t req_len3 = wire_writer3.pos();
        bool sent3 = i2c_send(wire, NOTECARD_I2C_ADDR,
            reinterpret_cast<const uint8_t*>(wire_buf3), req_len3);
        REQUIRE(sent3);

        uint8_t resp3[512] = {};
        size_t resp_len3 = i2c_recv(wire, NOTECARD_I2C_ADDR, resp3, sizeof(resp3), 5000);
        REQUIRE(resp_len3 > 4);

        printf("  [CRC test] Post-CRC JSONB response (%zu bytes):", resp_len3);
        for (size_t i = 0; i < resp_len3; i++) printf(" %02x", resp3[i]);
        printf("\n");

        // Decode and parse
        const uint8_t* payload3 = resp3 + 2;
        size_t payload_len3 = resp_len3;
        while (payload_len3 > 0 && resp3[payload_len3 - 1] == '\n') payload_len3--;
        REQUIRE(payload_len3 >= 4);
        payload_len3 -= 4;

        std::vector<uint8_t> decoded3;
        note::CobsDecoder decoder3(note::jsonb::kCobsXor);
        decoder3.feed(payload3, payload_len3,
            [&](const uint8_t* data, size_t n) {
                decoded3.insert(decoded3.end(), data, data + n);
            });
        decoder3.flush([&](const uint8_t* data, size_t n) {
            decoded3.insert(decoded3.end(), data, data + n);
        });

        struct CrcProbeSink2 : note::JsonSink {
            bool found_crc = false;
            std::string crc_value;
            std::vector<std::string> all_fields;

            void on_string(note::string_view k, note::string_view v) override {
                all_fields.emplace_back(k.data(), k.size());
                if (k == "crc") { found_crc = true; crc_value.assign(v.data(), v.size()); }
            }
            void on_int(note::string_view k, note::json_int_t) override {
                all_fields.emplace_back(k.data(), k.size());
                if (k == "crc") found_crc = true;
            }
            void on_float(note::string_view k, double) override {
                all_fields.emplace_back(k.data(), k.size());
                if (k == "crc") found_crc = true;
            }
            void on_bool(note::string_view k, bool) override {
                all_fields.emplace_back(k.data(), k.size());
                if (k == "crc") found_crc = true;
            }
        } sink3;

        struct VectorReader3 {
            const std::vector<uint8_t>& data;
            size_t pos = 0;
            note::Result<size_t> operator()(uint8_t* buf, size_t max, uint32_t) {
                if (pos >= data.size()) return size_t(0);
                size_t n = max < (data.size() - pos) ? max : (data.size() - pos);
                memcpy(buf, data.data() + pos, n);
                pos += n;
                return n;
            }
        } reader3{decoded3};

        char storage3[512];
        note::SaxStreamBuf buf3(storage3);
        auto dispatch3 = note::make_sax_dispatch(sink3);
        auto err3 = note::jsonb_parse_streaming(reader3, 1000, buf3, dispatch3);

        printf("  [CRC test] Post-CRC fields:");
        for (auto& f : sink3.all_fields) printf(" '%s'", f.c_str());
        printf("\n");

        printf("\n  ============================================\n");
        printf("  RESULT (after CRC-enabled JSON exchange):\n");
        printf("  JSONB response %s a 'crc' field\n",
            sink3.found_crc ? "DOES contain" : "does NOT contain");
        if (sink3.found_crc) {
            printf("  CRC value: '%s'\n", sink3.crc_value.c_str());
        }
        printf("  ============================================\n\n");

        CHECK(err3.empty());
    }
}

#endif // NOTECARD_TEST_I2C
