// serial_basic — connect a Blues Notecard via UART and send sensor data to Notehub
//
// Wiring: Notecard TX/RX → board Serial1 RX/TX pins.
// Pin defaults are per-chip (ESP32-S3: RX1=15, TX1=16).
// Override with -DRX1=<pin> -DTX1=<pin> if your board differs.
//
// Dependencies: just note-cpp (header-only, no external JSON library needed).

#include <note.hpp>

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
    Serial.println("[note-cpp] serial_basic starting");

    nc.begin(Serial1, 9600);

    // hub.set is idempotent — safe to call on every boot.
    if (auto r = nc.hub.set()
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
