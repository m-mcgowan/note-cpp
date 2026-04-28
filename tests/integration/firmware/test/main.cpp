#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest.h>

// Determine which interfaces are available based on pin definitions.
#include "../include/hal_serial.hpp"
#include "../include/hal_i2c.hpp"
#include "../include/firmware_version.hpp"
#include "../include/notecard_api_fixture.hpp"

#if !defined(NOTECARD_TEST_SERIAL) && !defined(NOTECARD_TEST_I2C)
#error "No Notecard interface configured. Define RX1/TX1 for serial and/or NOTECARD_I2C_SDA/SCL for I2C."
#endif

#include <note/notecard.hpp>
#include <note/api.hpp>
#ifdef NOTECARD_TEST_SERIAL
#include <note/transport/serial.hpp>
#endif
#ifdef NOTECARD_TEST_I2C
#include <note/transport/i2c.hpp>
#endif

// Globals — shared test fixture + firmware version gating.
note::Api<>* g_api = nullptr;
note::StreamingTransport* g_streaming_transport = nullptr;
int g_fw_version = 0;
static std::string g_fw_excludes;

#ifdef NOTECARD_TEST_SERIAL
static SerialHal& serial_hal() {
    static SerialHal hal{notecardUart()};
    return hal;
}
static note::transport::NotecardSerial<>& serial_transport() {
    static note::transport::NotecardSerial<> t{serial_hal()};
    return t;
}
static note::StreamingTransport& serial_streaming() {
    static note::StreamingTransport st{serial_transport()};
    return st;
}
#endif

#ifdef NOTECARD_TEST_I2C
TwoWire& notecardWire() {
    static TwoWire wire(0);
    return wire;
}
static Esp32I2CHal& i2c_hal() {
    static Esp32I2CHal hal{notecardWire()};
    return hal;
}
static note::transport::NotecardI2c<>& i2c_transport() {
    static note::transport::NotecardI2c<> t{i2c_hal()};
    return t;
}
static note::StreamingTransport& i2c_streaming() {
    static note::StreamingTransport st{i2c_transport()};
    return st;
}
#endif

static note::Notecard& get_notecard() {
#ifdef NOTECARD_TEST_SERIAL
    static note::Notecard nc(serial_streaming());
#elif defined(NOTECARD_TEST_I2C)
    static note::Notecard nc(i2c_streaming());
#endif
    return nc;
}

static note::Api<>& get_api() {
    static note::Api<> api(get_notecard());
    return api;
}

static void wire_debug(const note::WireEvent& ev, void*) {
    const char* dir = (ev.direction == note::WireDirection::Send) ? ">>>" : "<<<";
    Serial.printf("  [wire %s] %.*s\n", dir, (int)ev.json.size(), ev.json.data());
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

    // Initialize global Api for shared tests.
    g_api = &get_api();
#ifdef NOTECARD_TEST_SERIAL
    g_streaming_transport = &serial_streaming();
#elif defined(NOTECARD_TEST_I2C)
    g_streaming_transport = &i2c_streaming();
#endif

    // Enable wire debug — prints request/response JSON to serial log.
    note::DebugListener d{};
    d.on_wire = wire_debug;
    get_notecard().set_debug(d);

    // Probe firmware version via typed API (works with streaming transport).
    auto ver_rsp = get_api().card.version().execute();
    if (ver_rsp) {
        auto ver = note::string_view(ver_rsp.version);
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

#ifdef GCOV_ENABLED
extern "C" {
#  if defined(PIO_GCOV_TRACE_PC_BITMAP_BYTES)
#    include "pio_gcov_trace_pc.h"
#  else
#    include "gcov_serial.h"
#  endif
}
#endif

void setup() {
    etst::config.board_init = board_init;
    etst::doctest::config.configure = configure_context;
#ifdef GCOV_ENABLED
#  if defined(PIO_GCOV_TRACE_PC_BITMAP_BYTES)
    etst::config.after_cycle = pio_gcov_trace_pc_dump;
#  else
    etst::config.after_cycle = gcov_serial_dump;
#  endif
#endif
    DOCTEST_SETUP();
}
void loop()  { DOCTEST_LOOP(); }
