#pragma once
/// @file hal_i2c.hpp
/// ESP32 I2cHal implementation using Arduino Wire.
///
/// Guarded by NOTECARD_I2C_SDA / NOTECARD_I2C_SCL pin definitions.
/// When both are defined, NOTECARD_TEST_I2C is set and the HAL class
/// is available. When neither is defined, this header is a no-op.

#if defined(NOTECARD_I2C_SDA) && defined(NOTECARD_I2C_SCL)
#define NOTECARD_TEST_I2C 1

#include <note/transport/i2c.hpp>
#include <Wire.h>

class Esp32I2cHal : public note::transport::I2cHal {
    TwoWire& wire_;
    uint8_t addr_;
    int sda_;
    int scl_;
public:
    explicit Esp32I2cHal(TwoWire& wire,
                         int sda = NOTECARD_I2C_SDA,
                         int scl = NOTECARD_I2C_SCL,
                         uint8_t addr = note::transport::kI2cDefaultAddress)
        : wire_(wire), addr_(addr), sda_(sda), scl_(scl)
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

    bool transmit(const uint8_t* data, size_t len) override {
        // Notecard I2C protocol: first byte is the data length, followed by data.
        wire_.beginTransmission(addr_);
        wire_.write(static_cast<uint8_t>(len));
        size_t written = wire_.write(data, len);
        uint8_t err = wire_.endTransmission();
        return err == 0 && written == len;
    }

    bool receive(uint8_t* buf, size_t len, uint32_t& available) override {
        // Notecard I2C protocol: write [0x00, request_len], then read response.
        wire_.beginTransmission(addr_);
        wire_.write(static_cast<uint8_t>(0x00));
        wire_.write(static_cast<uint8_t>(len));
        if (wire_.endTransmission() != 0) {
            return false;
        }

        // Read: 2-byte header [available, good_bytes] + data
        size_t request_bytes = 2 + len;
        size_t got = wire_.requestFrom(addr_, request_bytes);
        if (got < 2) {
            return false;
        }

        available = static_cast<uint32_t>(wire_.read());
        uint8_t good_bytes = static_cast<uint8_t>(wire_.read());

        // Read only the valid bytes the Notecard actually returned
        for (uint8_t i = 0; i < good_bytes && wire_.available(); ++i) {
            buf[i] = static_cast<uint8_t>(wire_.read());
        }
        // Drain any remaining bytes from the Wire buffer
        while (wire_.available()) {
            wire_.read();
        }
        return true;
    }

    uint32_t millis() override { return ::millis(); }
    void delay(uint32_t ms) override { ::delay(ms); }
    size_t max_transfer() override { return 253; }
};

#endif // NOTECARD_TEST_I2C
