#pragma once

#include <note/transport/i2c.hpp>

#include <Arduino.h>
#include <Wire.h>

// note::arduino::I2CHal
//
// Implements note::transport::I2CHal for Arduino TwoWire (Wire library).
//
// The Notecard uses a Serial-over-I2C (SoI2C) framing protocol on top of
// raw I2C. This HAL implements that framing; note::transport::NotecardI2c
// handles the higher-level Notecard JSON protocol (CRC, retry, reset sync).
//
// SoI2C wire format:
//   transmit: [size_byte, data...]
//   receive:  write [0x00, N] then read [available, N, data[0..N-1]]
//             (priming query: write [0x00, 0x00] then read [available, 0x00])
//
// Usage:
//
//   note::arduino::I2CHal hal(Wire);           // default address 0x17
//   note::arduino::I2CHal hal(Wire, 0x17, 30); // explicit address + MTU
//   note::transport::NotecardI2c transport(hal);
//   note::Notecard nc(backend,
//       [&transport](note::string_view req, uint32_t t) {
//           return transport(req, t);
//       });

namespace note::arduino {

class I2CHal : public note::transport::I2CHal {
public:
    // SoI2C response header size: [available, echo_of_requested_count]
    static constexpr uint8_t kResponseHeaderSize = 2;

    explicit I2CHal(TwoWire& wire,
                    uint8_t  address     = note::transport::kI2cDefaultAddress,
                    size_t   max_xfer    = note::transport::kI2cDefaultMtu)
        : wire_(wire), address_(address), max_xfer_(max_xfer) {
        wire_.begin();
    }

    // Hardware reset: cycle the Wire bus.
    bool reset() override {
#if WIRE_HAS_END
        wire_.end();
#endif
        wire_.begin();
        return true;
    }

    // Transmit len bytes to the Notecard.
    // SoI2C TX format: [size_byte, data...]
    bool transmit(const uint8_t* data, size_t len) override {
        wire_.beginTransmission(address_);
        wire_.write(static_cast<uint8_t>(len));
        wire_.write(data, static_cast<size_t>(len));
        return wire_.endTransmission() == 0;
    }

    // Receive from the Notecard.
    //   len == 0: priming query — write [0x00, 0x00], read [available, 0x00].
    //   len == N: write [0x00, N], read [available, N, data[0..N-1]].
    // SoI2C RX format: write request header then requestFrom len+2 bytes.
    bool receive(uint8_t* buf, size_t len, uint32_t& available) override {
        // Step 1: write the request header to tell the Notecard how many
        // bytes we want.
        wire_.beginTransmission(address_);
        wire_.write(static_cast<uint8_t>(0));         // request type (always 0)
        wire_.write(static_cast<uint8_t>(len));        // number of bytes requested
        if (wire_.endTransmission() != 0) return false;

        // Brief delay: allows the Notecard I2C ISR to prepare the response.
        ::delay(2);

        // Step 2: read len + 2 bytes (2-byte response header + payload).
        const int expected = static_cast<int>(len) + kResponseHeaderSize;
        const int got = wire_.requestFrom(static_cast<int>(address_), expected);
        if (got != expected) return false;

        // Response header byte 0: number of bytes still available after this read.
        available = static_cast<uint32_t>(wire_.read());

        // Response header byte 1: echo of the requested byte count.
        // Discard — could be validated against len for robustness.
        wire_.read();

        // Payload.
        for (size_t i = 0; i < len; ++i) {
            buf[i] = static_cast<uint8_t>(wire_.read());
        }

        return true;
    }

    uint32_t millis()          override { return ::millis(); }
    void     delay(uint32_t ms) override { ::delay(ms); }
    size_t   max_transfer()    override { return max_xfer_; }

private:
    TwoWire& wire_;
    uint8_t  address_;
    size_t   max_xfer_;
};

}  // namespace note::arduino
