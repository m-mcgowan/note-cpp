// Minimal firmware for partition table upload via `pio run -t upload`.
// The actual test code lives in test/main.cpp and runs via `pio test`.
#include <Arduino.h>

void setup() {
    Serial.begin(115200);
    delay(2000);
    Serial.println("note-cpp integration test firmware (idle)");
}

void loop() {
    delay(1000);
}
