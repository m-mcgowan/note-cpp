#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest.h>

// Determine which interfaces are available based on pin definitions.
#include "../include/hal_serial.hpp"
#include "../include/hal_i2c.hpp"
#include "../include/firmware_version.hpp"

#if !defined(NOTECARD_TEST_SERIAL) && !defined(NOTECARD_TEST_I2C)
#error "No Notecard interface configured. Define RX1/TX1 for serial and/or NOTECARD_I2C_SDA/SCL for I2C."
#endif

#include <note/notecard.hpp>
#include <note/backends/cjson.hpp>
#ifdef NOTECARD_TEST_SERIAL
#include <note/transport/serial.hpp>
#endif
#ifdef NOTECARD_TEST_I2C
#include <note/transport/i2c.hpp>
#endif

// Global firmware version — tests can read this.
int g_fw_version = 0;

// Suite excludes built from firmware version — consumed by TEST_EXCLUDE_SUITE.
static std::string g_fw_excludes;

static int probe_notecard_version() {
    note::backends::CjsonBackend backend;

#ifdef NOTECARD_TEST_SERIAL
    static SerialHal serial_hal{notecardUart()};
    static note::transport::NotecardSerial serial_transport{serial_hal};
    note::Notecard nc(backend, serial_transport);
#elif defined(NOTECARD_TEST_I2C)
    static TwoWire& wire = *[]() -> TwoWire* { static TwoWire w(0); return &w; }();
    static Esp32I2CHal i2c_hal{wire};
    static note::transport::NotecardI2c i2c_transport{i2c_hal};
    note::Notecard nc(backend, i2c_transport);
#endif

    auto rsp = nc.request("card.version");
    if (rsp) {
        auto ver = (*rsp)->get_string("version");
        Serial.printf("  Firmware: %.*s\n", (int)ver.size(), ver.data());
        return parse_firmware_version(ver);
    }
    Serial.println("  WARNING: card.version failed — no version gating");
    return 0;
}

static bool board_init(Print& log) {
    log.println("=== note-cpp integration tests ===");
#ifdef NOTECARD_TEST_SERIAL
    log.printf("  Serial: RX=%d TX=%d\n", (int)RX1, (int)TX1);
#else
    log.println("  Serial: not configured");
#endif
#ifdef NOTECARD_TEST_I2C
    log.printf("  I2C:    SDA=%d SCL=%d\n", NOTECARD_I2C_SDA, NOTECARD_I2C_SCL);
#else
    log.println("  I2C:    not configured");
#endif

    g_fw_version = probe_notecard_version();
    log.printf("  Version code: %d\n", g_fw_version);

    g_fw_excludes = build_version_excludes(g_fw_version);
    if (!g_fw_excludes.empty()) {
        log.printf("  Excluding: %s\n", g_fw_excludes.c_str());
    }
    log.println();
    return true;
}

#define PTR_BOARD_INIT board_init

// Apply firmware version-based suite excludes before tests run.
static void configure_context(doctest::Context& ctx) {
    if (!g_fw_excludes.empty()) {
        ctx.setOption("test-suite-exclude", g_fw_excludes.c_str());
    }
}
#define PTR_CONFIGURE_CONTEXT configure_context

#include <pio_test_runner/doctest_runner.h>

void setup() { DOCTEST_SETUP(); }
void loop()  { DOCTEST_LOOP(); }
