#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest.h>
#include <Arduino.h>

void setup() {
    Serial.begin(115200);
    // Give the serial monitor time to connect.
    delay(2000);

    doctest::Context ctx;
    ctx.setOption("no-breaks", true);   // don't break into debugger on failure
    int result = ctx.run();

    Serial.println();
    if (result == 0) {
        Serial.println("=== ALL TESTS PASSED ===");
    } else {
        Serial.printf("=== %d TEST(S) FAILED ===\n", result);
    }
}

void loop() {
    delay(1000);
}
