#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest.h>

#include "../include/notecard_api_fixture.hpp"
#include "../../firmware/include/firmware_version.hpp"

#include <note/notecard.hpp>
#include <note/api.hpp>
#include <note/backends/cjson.hpp>
#include <note/link/serial.hpp>

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "note_emu_arduino.h"
#include "note_emu_serial_hal.hpp"

// Globals for shared tests.
note::Api<>* g_api = nullptr;
int g_fw_version = 0;
static std::string g_fw_excludes;

// Persistent objects.
static WiFiClientSecure g_wifi_client;
static NoteEmuArduino* g_note_emu = nullptr;
static note_emu_t* g_emu = nullptr;

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

static note::Notecard& get_notecard() {
    static noteemu::SoftcardSerialHal hal(*g_emu, millis, delay);
    static note::link::SerialFramer<> transport(hal);
    static note::backends::CjsonBackend backend;
    static note::Notecard nc(backend, transport);
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
    static NoteEmuArduino note_emu(NOTEHUB_PAT, g_wifi_client);
    g_note_emu = &note_emu;

    note_emu_err_t err = note_emu.create(&g_emu);
    if (err != NOTE_EMU_OK) {
        log.printf("  FATAL: note_emu_create: %s\n", note_emu_strerror(err));
        return false;
    }
    log.println("  Softcard: connected");

    // Initialize global Api for shared tests.
    g_api = &get_api();

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

#define PTR_BOARD_INIT board_init

static void configure_context(doctest::Context& ctx) {
    if (!g_fw_excludes.empty()) {
        ctx.setOption("test-suite-exclude", g_fw_excludes.c_str());
    }
}
#define PTR_CONFIGURE_CONTEXT configure_context

#include <pio_test_runner/doctest_runner.h>

void setup() { DOCTEST_SETUP(); }
void loop()  { DOCTEST_LOOP(); }
