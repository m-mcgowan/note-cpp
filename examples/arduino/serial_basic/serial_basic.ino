// serial_basic — connect a Blues Notecard via UART and send sensor data to Notehub
//
// Wiring: Notecard TX/RX → board Serial1 RX/TX pins.
//
// Dependencies (PlatformIO / Arduino Library Manager):
//   - note-cpp   (this library — header-only)
//   - cJSON      JSON backend (bundled with ESP32 Arduino framework;
//                add bblanchon/cJSON via Library Manager for other boards)
//
// On ESP32 with FreeRTOS, avoid global Wire/HardwareSerial objects — move
// these declarations into setup() or use the lazy-init pattern shown in
// tests/integration/firmware/.

#include <note.hpp>
#include <note/backends/cjson.hpp>

using namespace note::literals;

// ── Sensor data ───────────────────────────────────────────────────────────────
// Plain aggregate — field names are reflected automatically on C++20.
// For C++17 toolchains add: NOTE_FIELDS(temperature, humidity)

struct Sensor {
    float   temperature;
    int32_t humidity;
    NOTE_FIELDS(temperature, humidity)
};

// ── Notecard stack ─────────────────────────────────────────────────────────────

note::arduino::SerialHal      hal(Serial1);     // wraps HardwareSerial
note::transport::NotecardSerial transport(hal); // Notecard serial protocol
note::backends::CjsonBackend  backend;
note::Notecard                notecard(backend, transport);
note::Api                     api(notecard);

// ──────────────────────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 10000) delay(10);  // wait for USB CDC
    Serial.println("[note-cpp] serial_basic starting");

    // hub.set is idempotent — safe to call on every boot.
    if (auto r = api.hub.set()
            .product("com.example.myproject:mydevice")
            .mode("periodic")
            .outbound(15_mins)
            .execute();
        !r) {
        Serial.print("[error] hub.set: ");
        Serial.println(r.error().message.data());
    } else {
        Serial.println("[ok] hub.set");
    }
}

void loop() {
    Sensor reading{.temperature = 22.5f, .humidity = 60};

    if (auto r = api.note.add()
            .file("sensors.qo")
            .body(reading)
            .execute();
        !r) {
        Serial.print("[error] note.add: ");
        Serial.println(r.error().message.data());
    } else {
        Serial.println("[ok] note.add");
    }

    delay(60'000);
}
