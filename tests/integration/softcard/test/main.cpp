#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest.h>

// Arduino networking / TLS headers must precede the note-cpp headers (pulled
// in transitively by the fixture below) so the ESP32 mbedtls/PSA config is
// established before note-cpp's md5.hpp includes <mbedtls/md5.h>. Otherwise
// PSA's crypto_builtin_primitives.h can't resolve mbedtls_md5_context.
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "../include/notecard_api_fixture.hpp"
#include "../../firmware/include/firmware_version.hpp"

#include <note/notecard.hpp>
#include <note/api.hpp>
#include <note/link/serial.hpp>

#include <note/emu/arduino.hpp>
#include <note/emu/serial_hal.hpp>

// Globals for shared tests.
note::Api<>* g_api = nullptr;
note::Protocol* g_streaming_transport = nullptr;
int g_fw_version = 0;
static std::string g_fw_excludes;

// Persistent objects.
static WiFiClientSecure g_wifi_client;
static note::emu::Arduino* g_softcard = nullptr;

static bool connect_wifi(Print& log) {
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    log.print("  WiFi...");
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start > 30000) {
            log.println(" TIMEOUT");
            return false;
        }
        delay(500);
        log.print(".");
    }
    log.printf(" %s\n", WiFi.localIP().toString().c_str());
    return true;
}

static note::emu::SerialHal& emu_hal() {
    static note::emu::SerialHal hal(*g_softcard->instance(), millis, delay);
    return hal;
}
static note::link::SerialFramer<>& emu_transport() {
    static note::link::SerialFramer<> t{emu_hal()};
    return t;
}
static note::Protocol& emu_streaming() {
    static note::Protocol st{emu_transport()};
    return st;
}

static note::Notecard& get_notecard() {
    static note::Notecard nc(emu_streaming());
    return nc;
}

static note::Api<>& get_api() {
    static note::Api<> api(get_notecard());
    return api;
}

static bool board_init(Print& log) {
    log.println("=== note-cpp softcard integration tests ===");

    // Connect WiFi.
    if (!connect_wifi(log)) return false;

    // Allow connections to softcard.blues.com without a root CA.
    g_wifi_client.setInsecure();

    // Create note-emu instance (resolves account UID from PAT).
    static note::emu::Arduino softcard(NOTEHUB_PAT);
    g_softcard = &softcard;

    note_emu_err_t err = softcard.begin(g_wifi_client);
    if (err != NOTE_EMU_OK) {
        log.printf("  FATAL: note_emu begin: %s\n", note_emu_strerror(err));
        return false;
    }
    log.println("  Softcard: connected");

    // Initialize globals for shared tests.
    g_api = &get_api();
    g_streaming_transport = &emu_streaming();

    // Probe firmware version.
    auto rsp = get_notecard().request("card.version");
    if (rsp) {
        auto ver = (*rsp)->get_string("version");
        log.printf("  Firmware: %.*s\n", (int)ver.size(), ver.data());
        g_fw_version = parse_firmware_version(ver);
    } else {
        log.println("  WARNING: card.version failed — no version gating");
    }
    log.printf("  Version code: %d\n", g_fw_version);

    g_fw_excludes = build_version_excludes(g_fw_version);
    if (!g_fw_excludes.empty()) {
        log.printf("  Excluding: %s\n", g_fw_excludes.c_str());
    }
    log.println();
    return true;
}

static void configure_context(doctest::Context& ctx) {
    if (!g_fw_excludes.empty()) {
        ctx.setOption("test-suite-exclude", g_fw_excludes.c_str());
    }
}

#include <etst/doctest/runner.h>

void setup() {
    etst::config.board_init = board_init;
    etst::doctest::config.configure = configure_context;
    DOCTEST_SETUP();
}
void loop()  { DOCTEST_LOOP(); }
