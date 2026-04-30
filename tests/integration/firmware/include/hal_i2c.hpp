#pragma once
/// @file hal_i2c.hpp
/// ESP32 I2cHal subclass for the integration test rig.
///
/// On ESP32 boards, the Notecard sits on non-default I2C pins
/// (NOTECARD_I2C_SDA / NOTECARD_I2C_SCL — supplied via build flags) and
/// the Wire library's default buffer (128 B) is too small to hold the
/// 253-byte MTU + 3-byte protocol header. This subclass:
///
///   1. Uses note::arduino::external_bus so the base class never calls
///      Wire.begin() / Wire.end() — bus management stays here.
///   2. Calls wire_.setBufferSize(256) before every Wire.begin(), so the
///      ESP32 driver allocates a buffer large enough for an MTU read.
///   3. Calls Wire.begin(sda, scl) on construction and after reset().
///
/// When neither pin is defined, NOTECARD_TEST_I2C is left unset and this
/// header is a no-op.

#if defined(NOTECARD_I2C_SDA) && defined(NOTECARD_I2C_SCL)
#define NOTECARD_TEST_I2C 1

#include <note/arduino/i2c.hpp>

class Esp32I2cHal : public note::arduino::I2cHal {
    int sda_;
    int scl_;
public:
    explicit Esp32I2cHal(TwoWire& wire,
                         int sda = NOTECARD_I2C_SDA,
                         int scl = NOTECARD_I2C_SCL,
                         uint8_t addr = note::link::kI2cDefaultAddress)
        : note::arduino::I2cHal(wire, note::arduino::external_bus, addr, 253)
        , sda_(sda), scl_(scl)
    {
        wire_.setBufferSize(256);
        wire_.begin(sda_, scl_);
    }

    bool reset() override {
        wire_.end();
        wire_.setBufferSize(256);
        wire_.begin(sda_, scl_);
        return true;
    }
};

#endif // NOTECARD_TEST_I2C
