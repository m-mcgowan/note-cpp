#pragma once

#include <note/link/serial.hpp>

#include <note/arduino/compat.hpp>
#include <algorithm>

// note::arduino::SerialHal
//
// Implements note::link::SerialHal for any Arduino HardwareSerial
// (or compatible Stream subclass with write/readBytes/available).
//
// Usage — full stack (SerialHal → SerialFramer → Protocol → Notecard):
//
//   note::arduino::SerialHal hal(Serial1, 9600);
//   note::link::SerialFramer serial_hal(hal);   // note::Hal — wire framing
//   note::Protocol transport(serial_hal);        // ITransact
//   note::Notecard nc(backend, transport);
//
// For a convenience wrapper that owns all three layers, see
// note::arduino::SerialTransportStack in <note/arduino/begin.hpp>.

namespace note::arduino {

template <typename SerialT = HardwareSerial>
#if NOTE_STATIC_HAL
class SerialHal {
#else
class SerialHal : public note::link::SerialHal {
#endif
public:
    // Lazy Serial.begin: deferred to first transmit/receive. This
    // matters on Wokwi's Uno simulation, where `uart_.begin(baud)`
    // called during C++ static init (before Arduino's `init()` runs
    // `sei()`) silently breaks the virtual USART — bytes transmit,
    // but RX-complete interrupts never fire. On real AVR hardware
    // the deferred begin is equivalent to static-time begin; on
    // non-AVR cores the compiler folds the branch away after the
    // first call.
    SerialHal(SerialT& uart, unsigned long baud = 9600)
        : uart_(uart), baud_(baud) {}

    // Send all len bytes synchronously. Flushes after write to ensure
    // the Notecard receives complete segments before any delay.
    bool transmit(const uint8_t* data, size_t len) NOTE_HAL_OVERRIDE {
        ensure_begun();
        const size_t written = uart_.write(data, len);
        uart_.flush();
        return written == len;
    }

    size_t receive(uint8_t* buf, size_t max_len) NOTE_HAL_OVERRIDE {
        ensure_begun();
        const size_t avail = static_cast<size_t>(uart_.available());
        if (avail == 0) return 0;
        return uart_.readBytes(buf, std::min(avail, max_len));
    }

    uint32_t millis() NOTE_HAL_OVERRIDE { return ::millis(); }
    void delay(uint32_t ms) NOTE_HAL_OVERRIDE { ::delay(ms); }

private:
    // always_inline keeps the hot-path branch as two or three AVR
    // instructions at every call site instead of an outlined function
    // call. The cold begin() branch still outlines normally.
    __attribute__((always_inline)) inline
    void ensure_begun() {
        if (!begun_) { uart_.begin(baud_); begun_ = true; }
    }

    SerialT&      uart_;
    unsigned long baud_;
    bool          begun_ = false;
};

}  // namespace note::arduino
