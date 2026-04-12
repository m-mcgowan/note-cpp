// i2c_basic — connect a Blues Notecard via I2C and send sensor data to Notehub
//
// Wiring: Notecard SDA/SCL → board I2C pins.
// Default I2C address: 0x17.
//
// Dependencies: just note-cpp (header-only, no external JSON library needed).

#include <note.hpp>
#include <nonstdlibcpp.hpp> // required only on AVR platforms


// ── Sensor data ──────────────────────────────────────────────────────────
struct Sensor {
    float   temperature;
    int32_t humidity;
    NOTE_FIELDS(temperature, humidity)
};

// ── Notecard ─────────────────────────────────────────────────────────────
Notecard nc;

void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 10000) delay(10);  // wait for USB CDC
    Serial.println("[note-cpp] i2c_basic starting");

    nc.begin(Wire);

    auto r = nc.hub.set()
        .product("com.example.myproject:mydevice")
        .mode("periodic")
        .outbound(15_mins)
        .execute();
    if (r) {
        Serial.println("[ok] hub.set");
    } else {
        Serial.print("[error] hub.set: ");
        Serial.println(r.error().message.data());
    }
}

void loop() {
    Sensor reading{.temperature = 22.5f, .humidity = 60};

    if (auto r = nc.note.add()
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
