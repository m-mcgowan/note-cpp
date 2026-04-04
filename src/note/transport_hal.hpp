#pragma once

/// @file transport_hal.hpp
/// TransportHal — hardware abstraction for Notecard byte transport.
///
/// Subclasses implement the four primitives: transmit, read, reset,
/// write_line_terminator. Protocol logic (retry, CRC, JSON framing)
/// lives in StreamingTransport / BufferedTransport, not here.

#include <note/error.hpp>
#include <note/types.hpp>

#include <cstddef>
#include <cstdint>

namespace note {

struct TransportHal {
    virtual ~TransportHal() = default;

    /// Send raw bytes. Returns false on hardware error.
    virtual bool transmit(const uint8_t* data, size_t len) = 0;

    /// Read available bytes (up to max_len) into buf.
    ///
    /// Blocks up to timeout_ms for at least one byte. On success, returns
    /// the number of bytes read (always > 0). On timeout or hardware error,
    /// returns an error.
    ///
    /// Returning 0 is discouraged — callers treat it as end-of-stream.
    /// Implementations should return an error on timeout instead.
    virtual Result<size_t> read(uint8_t* buf, size_t max_len, uint32_t timeout_ms) = 0;

    /// Reset the hardware to a known state. Returns true when ready.
    virtual bool reset() = 0;

    /// Write the protocol line terminator after a request.
    /// Serial: \r\n. I2C: \n.
    virtual bool write_line_terminator() = 0;

    /// Monotonic millisecond counter (wraps after ~49 days).
    /// Used by the Notecard layer for inter-transaction timing and retry
    /// budget tracking.
    virtual uint32_t millis() = 0;

    /// Platform delay. Here for convenience to avoid a separate platform
    /// abstraction — the HAL already knows the platform. May evolve into
    /// a broader platform hooks interface (e.g. watchdog feeding on ESP32
    /// during long delays).
    virtual void delay(uint32_t ms) = 0;
};

} // namespace note
