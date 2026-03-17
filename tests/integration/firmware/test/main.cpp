#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest.h>
#include <Arduino.h>

// Determine which interfaces are available based on pin definitions.
#include "../include/hal_serial.hpp"
#include "../include/hal_i2c.hpp"

#if !defined(NOTECARD_TEST_SERIAL) && !defined(NOTECARD_TEST_I2C)
#error "No Notecard interface configured. Define NOTECARD_SERIAL_RX/TX for serial and/or NOTECARD_I2C_SDA/SCL for I2C."
#endif

void setup() {
    Serial.begin(115200);
    // Give the serial monitor time to connect.
    delay(2000);

    Serial.println("=== note-cpp integration tests ===");
#ifdef NOTECARD_TEST_SERIAL
    Serial.printf("  Serial: RX=%d TX=%d\n", NOTECARD_SERIAL_RX, NOTECARD_SERIAL_TX);
#else
    Serial.println("  Serial: not configured");
#endif
#ifdef NOTECARD_TEST_I2C
    Serial.printf("  I2C:    SDA=%d SCL=%d\n", NOTECARD_I2C_SDA, NOTECARD_I2C_SCL);
#else
    Serial.println("  I2C:    not configured");
#endif
    Serial.println();

    doctest::Context ctx;
    ctx.setOption("no-breaks", true);    // don't break into debugger on failure
    ctx.setOption("success", true);      // print all assertions (PlatformIO needs this to count tests)
    ctx.setOption("no-exitcode", true);  // let PlatformIO handle pass/fail via output parsing
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
