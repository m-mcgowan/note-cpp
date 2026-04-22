// Quick start — connect to a Blues Notecard, configure Notehub, send
// a sensor reading, and read the Notecard's firmware version.
//
// Wiring (ESP32-S3 defaults): Notecard TX → RX1 (pin 15),
//                             Notecard RX → TX1 (pin 16).
// Or swap nc.begin(Serial1, 9600) for nc.begin(Wire) to use I2C.

#include <note.hpp>

struct Readings {
    float   temperature;
    int16_t humidity;
    NOTE_FIELDS(temperature, humidity)   // optional on C++20
};

Notecard nc;

void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 5000) delay(10);   // wait for USB CDC

    nc.begin(Serial1, 9600);

    nc.hub.set()
        .product("com.example.myproject:mydevice")
        .mode("periodic")
        .outbound(60_mins)
        .execute();

    Readings r{.temperature = 22.5f, .humidity = 60};
    nc.note.add()
       .file("sensors.qo")
       .body(r)
       .execute();

    auto rsp = nc.card.version().execute();
    if (rsp) {
        Serial.print("version: "); Serial.println(rsp.version);
    } else {
        Serial.println(rsp.error());
    }
}

void loop() {
    delay(60000);
}
