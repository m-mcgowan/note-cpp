/// @file test_jsonb_serial_spike.cpp
/// JSONB integration tests over serial UART — both raw spike and library-based.

#include "../include/hal_serial.hpp"
#ifdef NOTECARD_TEST_SERIAL

#include <doctest.h>
#include <cstdio>

#include <note/jsonb.hpp>
#include <note/transport/cobs.hpp>
#include <note/transport/detail/crc32.hpp>
#include <note/json_sax.hpp>
#include <note/json_sax_streaming.hpp>
#include <note/lexer/sax_adapter.hpp>

extern "C" {
#include "jsonb/jsonb.h"
}

// Read a newline-terminated response from UART with timeout.
static size_t serial_recv(HardwareSerial& uart, uint8_t* buf, size_t buf_size, uint32_t timeout_ms) {
    size_t len = 0;
    uint32_t deadline = millis() + timeout_ms;
    while (millis() < deadline && len < buf_size) {
        if (uart.available()) {
            uint8_t c = uart.read();
            buf[len++] = c;
            if (c == '\n') break;
        } else {
            delay(10);
        }
    }
    return len;
}

TEST_CASE("JSONB serial spike: card.version") {
    auto& uart = notecardUart();
    uart.begin(9600, SERIAL_8N1, RX1, TX1);
    delay(500);

    // Drain any stale data
    while (uart.available()) uart.read();

    SUBCASE("sanity: JSON card.version over serial") {
        const char* json_req = "{\"req\":\"card.version\"}\n";
        uart.write((const uint8_t*)json_req, strlen(json_req));
        uart.flush();

        uint8_t resp[512] = {};
        size_t resp_len = serial_recv(uart, resp, sizeof(resp), 5000);
        resp[resp_len] = '\0';

        printf("  JSON response (%zu bytes): %s", resp_len, (char*)resp);
        REQUIRE(resp_len > 0);
        CHECK(strstr((char*)resp, "version") != nullptr);
    }

    delay(500);
    while (uart.available()) uart.read();

    SUBCASE("spike: JSONB card.version (C library)") {
        uint8_t buf[256] = {};
        jsonbContext ctx;

        jsonbObjectBegin(&ctx, buf, sizeof(buf), nullptr);
        jsonbAddStringToObject(&ctx, "req", "card.version");
        uint32_t len = jsonbObjectEnd(&ctx);
        REQUIRE(len > 0);

        printf("  JSONB request (%u bytes):", len);
        for (uint32_t i = 0; i < len; i++) printf(" %02x", buf[i]);
        printf("\n");

        uart.write(buf, len);
        uart.flush();

        uint8_t resp[512] = {};
        size_t resp_len = serial_recv(uart, resp, sizeof(resp), 5000);

        printf("  Response (%zu bytes):", resp_len);
        for (size_t i = 0; i < resp_len && i < 80; i++) printf(" %02x", resp[i]);
        printf("\n");

        if (resp_len > 0 && jsonbPresent(resp, resp_len)) {
            printf("  Response IS JSONB!\n");
            jsonbContext rsp_ctx;
            if (jsonbParse(&rsp_ctx, resp, resp_len)) {
                printf("  version: %s\n", jsonbGetString(&rsp_ctx, "version"));
                printf("  device:  %s\n", jsonbGetString(&rsp_ctx, "device"));
                CHECK(strlen(jsonbGetString(&rsp_ctx, "version")) > 0);
            }
        }
        CHECK(resp_len > 0);
    }

    delay(500);
    while (uart.available()) uart.read();

    SUBCASE("library: JSONB card.version via note-cpp") {
        // Build framed JSONB request
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

        uart.write(reinterpret_cast<const uint8_t*>(wire_buf), req_len);
        uart.flush();

        uint8_t resp[512] = {};
        size_t resp_len = serial_recv(uart, resp, sizeof(resp), 5000);

        printf("  Response (%zu bytes):", resp_len);
        for (size_t i = 0; i < resp_len && i < 64; i++) printf(" %02x", resp[i]);
        if (resp_len > 64) printf(" ...");
        printf("\n");

        REQUIRE(resp_len > 4);
        REQUIRE(resp[0] == '{');
        REQUIRE(resp[1] == ':');

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

        // Parse with note-cpp
        struct VersionSink : note::JsonSink {
            std::string version;
            std::string device;
            void on_string(note::string_view k, note::string_view v) override {
                if (k == "version") version.assign(v.data(), v.size());
                else if (k == "device") device.assign(v.data(), v.size());
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

TEST_CASE("JSONB CRC investigation: does Notecard include crc in JSONB responses? (serial)") {
    auto& uart = notecardUart();
    uart.begin(9600, SERIAL_8N1, RX1, TX1);
    delay(500);
    while (uart.available()) uart.read();

    // Step 1: Send JSON card.version WITHOUT CRC — baseline
    {
        const char* json_req = "{\"req\":\"card.version\"}\n";
        uart.write((const uint8_t*)json_req, strlen(json_req));
        uart.flush();

        uint8_t resp[512] = {};
        size_t resp_len = serial_recv(uart, resp, sizeof(resp), 5000);
        REQUIRE(resp_len > 0);
        resp[resp_len] = '\0';

        printf("  [CRC] JSON (no CRC) response (%zu bytes): %s", resp_len, (char*)resp);
        bool has_crc = (strstr((char*)resp, "\"crc\"") != nullptr);
        printf("  [CRC] Response has crc: %s\n", has_crc ? "YES" : "NO");
    }

    delay(500);
    while (uart.available()) uart.read();

    // Step 2: Send JSON card.version WITH CRC — like note-c does (always sends CRC).
    // note-c unconditionally adds CRC to every request; the Notecard echoes it
    // back if it supports CRC.
    bool notecard_supports_crc = false;
    {
        using namespace note::transport::detail;
        char buf[256];
        snprintf(buf, sizeof(buf), "{\"req\":\"card.version\"}");
        size_t len = strlen(buf);
        len = crc_add(buf, len, sizeof(buf), 1);  // seq=1
        buf[len++] = '\n';

        printf("  [CRC] JSON+CRC request: %.*s", (int)len, buf);
        uart.write((const uint8_t*)buf, len);
        uart.flush();

        uint8_t resp[512] = {};
        size_t resp_len = serial_recv(uart, resp, sizeof(resp), 5000);
        REQUIRE(resp_len > 0);
        resp[resp_len] = '\0';

        printf("  [CRC] JSON+CRC response (%zu bytes): %s", resp_len, (char*)resp);
        notecard_supports_crc = (strstr((char*)resp, "\"crc\"") != nullptr);
        printf("  [CRC] Response has crc: %s\n", notecard_supports_crc ? "YES" : "NO");

        if (!notecard_supports_crc) {
            printf("  [CRC] Notecard did not echo CRC even when we sent it.\n");
            printf("  [CRC] JSONB CRC is moot — Notecard doesn't support CRC.\n");
            // Don't skip — still report the finding
        }
    }

    delay(500);
    while (uart.available()) uart.read();

    // Step 3: Send JSONB card.version — check for crc field in JSONB response
    {
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
        printf("  [CRC] JSONB request (%zu bytes):", req_len);
        for (size_t i = 0; i < req_len; i++) printf(" %02x", (uint8_t)wire_buf[i]);
        printf("\n");

        uart.write(reinterpret_cast<const uint8_t*>(wire_buf), req_len);
        uart.flush();

        uint8_t resp[512] = {};
        size_t resp_len = serial_recv(uart, resp, sizeof(resp), 5000);
        REQUIRE(resp_len > 4);
        REQUIRE(resp[0] == '{');
        REQUIRE(resp[1] == ':');

        printf("  [CRC] JSONB response (%zu bytes):", resp_len);
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

        printf("  [CRC] Decoded JSONB (%zu bytes):", decoded.size());
        for (size_t i = 0; i < decoded.size(); i++) printf(" %02x", decoded[i]);
        printf("\n");

        // Parse and look for "crc" field
        struct CrcProbeSink : note::JsonSink {
            bool found_crc = false;
            std::string crc_value;
            std::vector<std::string> all_fields;

            void on_string(note::string_view k, note::string_view v) override {
                all_fields.emplace_back(k.data(), k.size());
                if (k == "crc") { found_crc = true; crc_value.assign(v.data(), v.size()); }
            }
            void on_int(note::string_view k, note::json_int_t) override {
                all_fields.emplace_back(k.data(), k.size());
            }
            void on_float(note::string_view k, double) override {
                all_fields.emplace_back(k.data(), k.size());
            }
            void on_bool(note::string_view k, bool) override {
                all_fields.emplace_back(k.data(), k.size());
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

        printf("  [CRC] Parse error: %s\n",
            err.empty() ? "(none)" : std::string(err).c_str());
        printf("  [CRC] Fields in JSONB response:");
        for (auto& f : sink.all_fields) printf(" '%s'", f.c_str());
        printf("\n");

        printf("\n  ============================================\n");
        printf("  RESULT: JSONB response %s a 'crc' field\n",
            sink.found_crc ? "DOES contain" : "does NOT contain");
        if (sink.found_crc) {
            printf("  CRC value: '%s'\n", sink.crc_value.c_str());
            printf("  -> note-cpp SHOULD support CRC for JSONB\n");
        } else {
            printf("  -> CRC is JSON-only; JSONB can safely skip CRC\n");
        }
        printf("  ============================================\n\n");

        CHECK(err.empty());
    }
}

#endif // NOTECARD_TEST_SERIAL
