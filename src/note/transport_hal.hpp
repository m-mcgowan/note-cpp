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
    virtual Result<size_t> read(uint8_t* buf, size_t max_len, uint32_t timeout_ms) = 0;

    /// Reset the hardware to a known state. Returns true when ready.
    virtual bool reset() = 0;

    /// Write the protocol line terminator after a request.
    virtual bool write_line_terminator() = 0;

    /// Monotonic millisecond counter (wraps after ~49 days).
    virtual uint32_t millis() = 0;

    /// Platform delay.
    virtual void delay(uint32_t ms) = 0;
};

} // namespace note
