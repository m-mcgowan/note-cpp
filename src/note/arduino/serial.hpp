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
#if NOTE_STATIC_HAL
class SerialHal {
#else
class SerialHal : public note::transport::SerialHal {
#endif
public:
    SerialHal(SerialT& uart, unsigned long baud = 9600)
        : uart_(uart), baud_(baud) {
        uart_.begin(baud_);
    }

    // Send all len bytes synchronously. Flushes after write to ensure
    // the Notecard receives complete segments before any delay.
    bool transmit(const uint8_t* data, size_t len) NOTE_HAL_OVERRIDE {
        const size_t written = uart_.write(data, len);
        uart_.flush();
        return written == len;
    }

    size_t receive(uint8_t* buf, size_t max_len) NOTE_HAL_OVERRIDE {
        const size_t avail = static_cast<size_t>(uart_.available());
        if (avail == 0) return 0;
        return uart_.readBytes(buf, std::min(avail, max_len));
    }

    uint32_t millis() NOTE_HAL_OVERRIDE { return ::millis(); }
    void delay(uint32_t ms) NOTE_HAL_OVERRIDE { ::delay(ms); }

private:
    SerialT&      uart_;
    unsigned long baud_;
};

}  // namespace note::arduino
