#pragma once
/// @file hal_i2c.hpp
/// I2C HAL alias for the integration tests.
///
/// Uses `note::arduino::I2cHal` directly with the production defaults
/// (default address 0x17, default MTU `note::link::kI2cDefaultMtu`). On
/// ESP32 the Notecard sits on non-default I2C pins, so the pin-aware
/// constructor is used — `Wire.begin(sda, scl)` is the only setup step.
///
/// Goal: validate the path a typical user takes. No subclass, no MTU
/// override, no `setBufferSize` — the integration tests stand in for
/// "did the default experience break on real hardware?".
///
/// When neither pin is defined, NOTECARD_TEST_I2C is left unset and
/// this header is a no-op.

#if defined(NOTECARD_I2C_SDA) && defined(NOTECARD_I2C_SCL)
#define NOTECARD_TEST_I2C 1

#include <note/arduino/i2c.hpp>

using NotecardI2cHal = note::arduino::I2cHal;

#endif // NOTECARD_TEST_I2C
