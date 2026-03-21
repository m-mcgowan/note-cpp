#pragma once
/// @file hal_serial.hpp
/// ESP32 SerialHal implementation using Arduino HardwareSerial.
///
/// Guarded by NOTECARD_SERIAL_RX / NOTECARD_SERIAL_TX pin definitions.
/// When both are defined, NOTECARD_TEST_SERIAL is set and the HAL class
/// is available. When neither is defined, this header is a no-op.

#if defined(NOTECARD_SERIAL_RX) && defined(NOTECARD_SERIAL_TX)
#define NOTECARD_TEST_SERIAL 1

#include <note/arduino/serial.hpp>

class Esp32SerialHal : public note::arduino::SerialHal<HardwareSerial> {
public:
    explicit Esp32SerialHal(HardwareSerial& uart,
                            int rx = NOTECARD_SERIAL_RX,
                            int tx = NOTECARD_SERIAL_TX,
                            unsigned long baud = 9600)
        : note::arduino::SerialHal<HardwareSerial>(uart, baud)
    {
        // Re-initialize with explicit RX/TX pins (overrides the base-class begin).
        uart.begin(baud, SERIAL_8N1, rx, tx);
    }
};

#endif // NOTECARD_TEST_SERIAL
