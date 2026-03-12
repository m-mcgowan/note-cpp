#pragma once
/// @file hal_i2c.hpp
/// ESP32 I2cHal implementation using Arduino Wire.

#include <note/transport/i2c.hpp>
#include <Wire.h>

#ifndef NOTECARD_I2C_SDA
#error "NOTECARD_I2C_SDA must be defined (source env.sh before building)"
#endif
#ifndef NOTECARD_I2C_SCL
#error "NOTECARD_I2C_SCL must be defined (source env.sh before building)"
#endif

class Esp32I2cHal : public note::transport::I2cHal {
    TwoWire& wire_;
    uint8_t addr_;
public:
    explicit Esp32I2cHal(TwoWire& wire,
                         int sda = NOTECARD_I2C_SDA,
                         int scl = NOTECARD_I2C_SCL,
                         uint8_t addr = note::transport::kI2cDefaultAddress)
        : wire_(wire), addr_(addr)
    {
        wire_.begin(sda, scl);
    }

    bool reset() override {
        wire_.end();
        wire_.begin();
        return true;
    }

    bool transmit(const uint8_t* data, size_t len) override {
        wire_.beginTransmission(addr_);
        size_t written = wire_.write(data, len);
        uint8_t err = wire_.endTransmission();
        return err == 0 && written == len;
    }

    bool receive(uint8_t* buf, size_t len, uint32_t& available) override {
        // SoI2C framing: write [0x00, request_len], then read response.
        wire_.beginTransmission(addr_);
        wire_.write(static_cast<uint8_t>(0x00));
        wire_.write(static_cast<uint8_t>(len));
        if (wire_.endTransmission() != 0) {
            return false;
        }

        // Read: 2-byte header + data
        size_t request_bytes = 2 + len;
        size_t got = wire_.requestFrom(addr_, request_bytes);
        if (got < 2) {
            return false;
        }

        available = static_cast<uint32_t>(wire_.read());
        wire_.read(); // discard echo byte

        for (size_t i = 0; i < len && wire_.available(); ++i) {
            buf[i] = static_cast<uint8_t>(wire_.read());
        }
        return true;
    }

    uint32_t millis() override { return ::millis(); }
    void delay(uint32_t ms) override { ::delay(ms); }
    size_t max_transfer() override { return 253; }
};
