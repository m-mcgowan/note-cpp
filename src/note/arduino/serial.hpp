#pragma once

#include <note/transport/serial.hpp>

#include <note/arduino/compat.hpp>
#include <algorithm>

// note::arduino::SerialHal
//
// Implements note::transport::SerialHal for any Arduino HardwareSerial
// (or compatible Stream subclass with write/readBytes/available).
//
// Usage:
//
//   note::arduino::SerialHal hal(Serial1, 9600);
//   note::transport::NotecardSerial transport(hal);
//   note::Notecard nc(backend,
//       [&transport](note::string_view req, uint32_t t) {
//           return transport(req, t);
//       });

namespace note::arduino {

template <typename SerialT = HardwareSerial>
class SerialHal : public note::transport::SerialHal {
public:
    SerialHal(SerialT& uart, unsigned long baud = 9600)
        : uart_(uart), baud_(baud) {
        uart_.begin(baud_);
    }

    // Send all len bytes synchronously. Flushes after write to ensure
    // the Notecard receives complete segments before any delay.
    bool transmit(const uint8_t* data, size_t len) override {
        const size_t written = uart_.write(data, len);
        uart_.flush();
        return written == len;
    }

    size_t receive(uint8_t* buf, size_t max_len) override {
        const size_t avail = static_cast<size_t>(uart_.available());
        if (avail == 0) return 0;
        return uart_.readBytes(buf, std::min(avail, max_len));
    }

    uint32_t millis() override { return ::millis(); }
    void delay(uint32_t ms) override { ::delay(ms); }

private:
    SerialT&      uart_;
    unsigned long baud_;
};

}  // namespace note::arduino
