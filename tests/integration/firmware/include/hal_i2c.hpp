#pragma once
/// @file hal_i2c.hpp
/// ESP32 I2CHal implementation using Arduino Wire.
///
/// Guarded by NOTECARD_I2C_SDA / NOTECARD_I2C_SCL pin definitions.
/// When both are defined, NOTECARD_TEST_I2C is set and the HAL class
/// is available. When neither is defined, this header is a no-op.

#if defined(NOTECARD_I2C_SDA) && defined(NOTECARD_I2C_SCL)
#define NOTECARD_TEST_I2C 1

#include <note/arduino/i2c.hpp>

class Esp32I2CHal : public note::arduino::I2CHal {
    int sda_;
    int scl_;
public:
    explicit Esp32I2CHal(TwoWire& wire,
                         int sda = NOTECARD_I2C_SDA,
                         int scl = NOTECARD_I2C_SCL,
                         uint8_t addr = note::transport::kI2cDefaultAddress)
        : note::arduino::I2CHal(wire, addr, 253)
        , sda_(sda), scl_(scl)
    {
        // Increase Wire buffer to support max_transfer (253 bytes) + protocol overhead.
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
