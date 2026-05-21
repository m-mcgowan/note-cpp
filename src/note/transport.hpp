#pragma once

/// @file transport.hpp
/// Layered transport — byte-channel half of the two-interface split.
///
///   IByteTransport — pure byte channel. Owns init handshake, CTX/RTX,
///     frame terminator detection, lookahead, raw HAL I/O. No JSON or
///     wire-format knowledge.
///
/// The wire-format half (`ITransact`) lives in `note/transact.hpp` and
/// sits on top of an `IByteTransport`. Concrete byte transports live in
/// `note/hal_byte_transport.hpp`; concrete wire-format transports in
/// `note/json_transact.hpp`.

#include <note/debug.hpp>
#include <note/error.hpp>
#include <note/transport_hal.hpp>
#include <note/types.hpp>

#include <cstdint>
#include <cstddef>

namespace note {

/// Pure byte-channel transport. Bracketed transaction over a duplex byte
/// stream — request bytes flow out, response bytes flow back.
///
/// Lifecycle:
///   1. begin_transaction(timeout_ms) — acquire bus, do CTX/RTX handshake
///      if the SKU has the pins, init the wire on first use.
///   2. zero or more write(data, len) — emit request bytes.
///   3. write_frame_terminator() — write the protocol-specific terminator
///      (`\r\n` for serial, `\n` for I2C). Transitions to receive state.
///   4. zero or more read(buf, max, timeout_ms) — consume response bytes:
///        Ok(N > 0)        : N bytes read
///        Ok(0)            : stream open, no data right now (non-blocking)
///        Err(EndOfFrame)  : clean response-frame boundary; no more bytes
///        Err(ResponseLost): transport corruption (intra-byte timeout, CRC
///                           mismatch enforced higher up, partial frame)
///        Err(<other>)     : real I/O error
///   5. end_transaction() — release bus, drain residue, debug events.
///      Must be called even on failure paths.
///
/// No JSON knowledge here. No CRC. No JSONB. Wire-format concerns live
/// one layer up in `ITransact`.
struct IByteTransport {
    virtual ~IByteTransport() = default;

    virtual Result<void> begin_transaction(uint32_t timeout_ms) = 0;
    virtual void          end_transaction() = 0;

    virtual Result<void> write(const uint8_t* data, size_t len) = 0;
    virtual Result<void> write_frame_terminator() = 0;

    virtual Result<size_t> read(uint8_t* buf, size_t max, uint32_t timeout_ms) = 0;

    virtual void reset() = 0;
    virtual void abort() = 0;
    virtual Hal& hal() = 0;
    virtual void set_debug(const DebugListener&) {}
};

} // namespace note
