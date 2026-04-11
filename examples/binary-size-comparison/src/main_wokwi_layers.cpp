// Layered Wokwi integration tests for note-cpp.
// Build with -DWOKWI_LAYER=N (1-4).

#if defined(WOKWI_LAYER)

#include <Arduino.h>

#if WOKWI_LAYER >= 1
#include <note/arduino/serial.hpp>
#endif
#if WOKWI_LAYER >= 2
#include <note/transport/serial.hpp>
#endif
#if WOKWI_LAYER >= 3
#include <note/streaming_transport.hpp>
#include <note/arduino/begin.hpp>
#endif
#if WOKWI_LAYER >= 4
#include <note/static_notecard.hpp>
#include <note/api.hpp>
#endif

// ── L1: HAL ───────────────────────────────────────────────────────────
#if WOKWI_LAYER == 1
void setup() {
    note::arduino::SerialHal<HardwareSerial> hal(Serial, 9600);
    const uint8_t nl = '\n';
    hal.transmit(&nl, 1);
    uint8_t buf[4];
    uint32_t start = hal.millis();
    size_t total = 0;
    while (hal.millis() - start < 2000 && total < 2) {
        size_t n = hal.receive(buf + total, sizeof(buf) - total);
        total += n;
        if (n == 0) hal.delay(1);
    }
    bool ok = (total >= 2 && buf[0] == '\r' && buf[1] == '\n');
    const char* msg = ok ? "PASS L1\n" : "FAIL L1\n";
    hal.transmit(reinterpret_cast<const uint8_t*>(msg), 8);
}
void loop() { delay(60000); }

// ── L2: Transport ─────────────────────────────────────────────────────
#elif WOKWI_LAYER == 2
void setup() {
    note::arduino::SerialHal<HardwareSerial> hal(Serial, 9600);
    note::transport::NotecardSerial<> nc(hal);
    bool ok = nc.reset();
    const char* msg = ok ? "PASS L2\n" : "FAIL L2\n";
    hal.transmit(reinterpret_cast<const uint8_t*>(msg), 8);
}
void loop() { delay(60000); }

// ── L3: StreamingTransport ────────────────────────────────────────────
#elif WOKWI_LAYER == 3
void setup() {
    note::arduino::SerialTransportStack<> stack(Serial, 9600);
    auto build = [](note::JsonBuilder& b, void*) {
        b.add("req", "hub.set");
    };
    note::JsonSink sink;
    auto result = stack.transport.transact(build, nullptr, sink, 10000);
    const char* msg = result ? "PASS L3\n" : "FAIL L3\n";
    Serial.write(msg);
}
void loop() { delay(60000); }

// ── L4: Full API execute with response validation ─────────────────────
// Sends card.version, parses the JSONB response, checks fields.
#elif WOKWI_LAYER == 4

#include <note/api/card_version.hpp>

using CardVersion = note::api::CardVersion;
alignas(4) static char arena_buf[CardVersion::Response::max_arena_size];
static note::MonotonicArena arena(arena_buf);

void setup() {
    note::StaticNotecard<note::arduino::SerialTransportStack<>> nc(
        note::arena_allocator(arena), Serial, 9600);
    note::Api api(nc);

    auto rsp = api.card.version().execute();
    if (!rsp) {
        Serial.write("FAIL L4\n");
        return;
    }
    if (!rsp.version.has_value() || !rsp.device.has_value()) {
        Serial.write("FAIL L4: fields\n");
        return;
    }
    Serial.write("PASS L4\n");
}
void loop() { delay(60000); }

#endif
#endif
