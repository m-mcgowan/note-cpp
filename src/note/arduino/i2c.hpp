#pragma once

#include <note/link/i2c.hpp>

#include <note/arduino/compat.hpp>
#include <Wire.h>

// note::arduino::I2cHal
//
// Implements note::link::I2cHal for Arduino TwoWire (Wire library).
//
// The Notecard uses a Serial-over-I2C (SoI2C) framing protocol on top of
// raw I2C. This HAL implements that framing; note::link::I2cFramer
// handles the higher-level Notecard JSON protocol (CRC, retry, reset sync).
//
// SoI2C wire format:
//   transmit: [size_byte, data...]
//   receive:  write [0x00, N] then read [available, good_bytes, data[0..good_bytes-1]]
//             (priming query: write [0x00, 0x00] then read [available, 0x00])
//
// Bus management:
//   By default the HAL "owns" the Wire bus — it calls Wire.begin() in its
//   constructor and Wire.end()/Wire.begin() inside reset(). This works on
//   devkit boards where Wire's default pins are correct and nothing else
//   shares the bus.
//
//   For non-devkit boards or shared buses, use one of:
//
//     I2cHal(Wire, sda, scl)             // HAL calls Wire.begin(sda, scl).
//     I2cHal(Wire, external_bus)         // App owns Wire; HAL never calls
//                                        // begin()/end().
//
// Usage — full stack (I2cHal → I2cFramer → Protocol → Notecard):
//
//   note::arduino::I2cHal hal(Wire);                 // default address 0x17
//   note::arduino::I2cHal hal(Wire, 14, 21);         // custom pins
//   note::arduino::I2cHal hal(Wire, note::arduino::external_bus);  // shared bus
//   note::link::I2cFramer i2c_hal(hal);          // note::Hal — wire framing
//   note::Protocol transport(i2c_hal);            // ITransact
//   note::Notecard nc(backend, transport);

namespace note::arduino {

/// Tag type selecting external bus management — see I2cHal docs.
struct ExternalBus {};
inline constexpr ExternalBus external_bus{};

class I2cHal : public note::link::I2cHal {
public:
    // SoI2C response header size: [available, good_bytes]
    static constexpr uint8_t kResponseHeaderSize = 2;

    /// Default: HAL calls Wire.begin() (no pin args) in the constructor and
    /// Wire.end()/Wire.begin() on reset. Use this on devkits where Wire's
    /// default pins are correct and nothing else shares the bus.
    explicit I2cHal(TwoWire& wire,
                    uint8_t  address     = note::link::kI2cDefaultAddress,
                    size_t   max_xfer    = note::link::kI2cDefaultMtu)
        : wire_(wire), address_(address), max_xfer_(max_xfer) {
        wire_.begin();
    }

    /// Pin-aware: HAL calls Wire.begin(sda, scl) in the constructor and
    /// Wire.end()/Wire.begin(sda, scl) on reset. Use this on boards where
    /// the Notecard sits on non-default I2C pins.
    I2cHal(TwoWire& wire, int sda, int scl,
           uint8_t  address     = note::link::kI2cDefaultAddress,
           size_t   max_xfer    = note::link::kI2cDefaultMtu)
        : wire_(wire), address_(address), max_xfer_(max_xfer),
          sda_(sda), scl_(scl) {
        wire_.begin(sda_, scl_);
    }

    /// External-bus: app owns Wire. The HAL never calls Wire.begin(),
    /// Wire.end(), or any other bus-init function. Use this when the bus
    /// is shared with other drivers/tasks, or when the app needs to retain
    /// full control over bus lifetime and pin configuration.
    I2cHal(TwoWire& wire, ExternalBus,
           uint8_t  address     = note::link::kI2cDefaultAddress,
           size_t   max_xfer    = note::link::kI2cDefaultMtu)
        : wire_(wire), address_(address), max_xfer_(max_xfer),
          manage_bus_(false) {}

    // Hardware reset: cycle the Wire bus, unless the app owns it.
    bool reset() override {
        if (!manage_bus_) {
            // App-managed bus: no-op. The app is responsible for any
            // recovery action it wants to take in response to a transient
            // I2C failure.
            return true;
        }
#if WIRE_HAS_END
        wire_.end();
#endif
        if (sda_ >= 0 && scl_ >= 0) {
            wire_.begin(sda_, scl_);
        } else {
            wire_.begin();
        }
        return true;
    }

    // Transmit len bytes to the Notecard.
    // SoI2C TX format: [size_byte, data...]
    bool transmit(const uint8_t* data, size_t len) override {
        wire_.beginTransmission(address_);
        wire_.write(static_cast<uint8_t>(len));
        size_t written = wire_.write(data, static_cast<size_t>(len));
        return wire_.endTransmission() == 0 && written == len;
    }

    // Receive from the Notecard.
    //   len == 0: priming query — write [0x00, 0x00], read [available, 0x00].
    //   len == N: write [0x00, N], read [available, good_bytes, data[0..good_bytes-1]].
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
        if (got < kResponseHeaderSize) return false;

        // Response header byte 0: number of bytes still available after this read.
        available = static_cast<uint32_t>(wire_.read());

        // Response header byte 1: number of valid payload bytes returned.
        // May be less than len if the Notecard has fewer bytes ready.
        const uint8_t good_bytes = static_cast<uint8_t>(wire_.read());

        // Copy only the valid bytes.
        for (uint8_t i = 0; i < good_bytes && wire_.available(); ++i) {
            buf[i] = static_cast<uint8_t>(wire_.read());
        }
        // Drain any leftover bytes from the Wire buffer.
        while (wire_.available()) {
            wire_.read();
        }

        return true;
    }

    uint32_t millis()           override { return ::millis(); }
    void     delay(uint32_t ms) override { ::delay(ms); }
    size_t   max_transfer()     override { return max_xfer_; }

protected:
    TwoWire& wire_;

private:
    uint8_t  address_;
    size_t   max_xfer_;
    int      sda_         = -1;
    int      scl_         = -1;
    bool     manage_bus_  = true;
};

}  // namespace note::arduino
