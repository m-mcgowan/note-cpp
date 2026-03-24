#pragma once
/// @file hal_serial.hpp
/// Serial HAL for integration tests.
///
/// Guarded by RX1/TX1 pin definitions (standard Arduino ESP32 defines).
/// Pass -DRX1=<pin> -DTX1=<pin> to configure. The Arduino framework
/// provides defaults per chip variant if not overridden.

#if defined(RX1) && defined(TX1)
#define NOTECARD_TEST_SERIAL 1

#include <note/arduino/serial.hpp>

// UART1 for Notecard (UART0 is the USB console).
// Lazy-init: HardwareSerial creates FreeRTOS primitives, so must not
// be constructed before the scheduler is ready.
inline HardwareSerial& notecardUart() {
    static HardwareSerial uart(1);
    return uart;
}

// arduino::SerialHal calls Serial1.begin(baud) which uses RX1/TX1 automatically.
using SerialHal = note::arduino::SerialHal<HardwareSerial>;

#endif // NOTECARD_TEST_SERIAL
