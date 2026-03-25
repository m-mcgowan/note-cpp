// i2c_basic — connect a Blues Notecard via I2C and send sensor data to Notehub
//
// Wiring: Notecard ESLOV/I2C port → board default SDA/SCL pins.
//
// Dependencies (PlatformIO / Arduino Library Manager):
//   - note-cpp   (this library — header-only)
//   - cJSON      JSON backend (bundled with ESP32 Arduino framework;
//                add bblanchon/cJSON via Library Manager for other boards)
//
// On ESP32 with FreeRTOS, avoid global Wire/HardwareSerial objects — move
// these declarations into setup() or use the lazy-init pattern shown in
// tests/integration/firmware/.

#include <note.hpp>  // Arduino library gateway — must be first
#include <note/arduino/i2c.hpp>
#include <note/backends/cjson.hpp>
#include <note/body.hpp>
#include <note/transport/i2c.hpp>

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

note::arduino::I2CHal         hal(Wire);        // wraps Arduino Wire (TwoWire)
note::transport::NotecardI2c  transport(hal);   // Notecard I2C protocol
note::backends::CjsonBackend  backend;
note::Notecard                notecard(backend, transport);
note::Api                     api(notecard);

// ──────────────────────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);

    // hub.set is idempotent — safe to call on every boot.
    if (auto r = api.hub.set()
            .product("com.example.myproject:mydevice")
            .mode("periodic")
            .outbound(15_mins)
            .execute();
        !r) {
        Serial.print("[error] hub.set: ");
        Serial.println(r.error().message.data());
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
    }

    delay(60'000);
}
