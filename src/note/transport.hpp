#pragma once

/// @file transport.hpp
/// SPIKE: layered transport design. Two interfaces:
///
///   IByteTransport — pure byte channel. Owns init handshake, CTX/RTX,
///     frame terminator detection, lookahead, raw HAL I/O. No JSON or
///     wire-format knowledge.
///
///   ITransact — request/response transactions with wire format. Owns
///     CRC, JSONB encode/decode, parser dispatch. Sits on top of an
///     IByteTransport; concrete impls are JsonTransact, JsonbTransact,
///     NoteCBridgeTransact.
///
/// The `note::spike::` namespace prevents collision with the existing
/// `note::ITransact` and `note::Protocol` during the design validation.
/// On approval the spike:: prefix is stripped and the old types are
/// replaced.

#include <note/debug.hpp>
#include <note/error.hpp>
#include <note/json.hpp>
#include <note/json_sax.hpp>
#include <note/request_source.hpp>
#include <note/span.hpp>
#include <note/transport_hal.hpp>
#include <note/types.hpp>

#include <cstdint>
#include <cstddef>

namespace note { namespace spike {

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

/// Request/response transaction interface — sits on top of an IByteTransport,
/// adds wire format (CRC, JSON or JSONB encoding/decoding, parser dispatch).
///
/// Two presentation shapes for the response:
///   - JsonSink&  — transport drives parsing inline, sink consumes SAX events.
///                  Zero-buffer; the dominant path for memory-constrained MCUs.
///   - span<char> — transport reads bytes into the caller's buffer, returns a
///                  string_view aliasing the response (JSON only — JSONB
///                  transports return NotReady from this shape since the
///                  decoded form is not raw JSON bytes).
struct ITransact {
    virtual ~ITransact() = default;

    virtual Result<void> transact(RequestSource src, JsonSink& sink,
                                  uint32_t timeout_ms) = 0;
    virtual Result<string_view> transact(RequestSource src, span<char> buf,
                                          uint32_t timeout_ms) = 0;
    virtual Result<void> send(RequestSource src) = 0;

    virtual void reset() = 0;
    virtual void abort() = 0;
    virtual Hal& hal() = 0;
    virtual void set_debug(const DebugListener&) {}

    /// Binary I/O for COBS streaming (card.binary). Passthrough to the
    /// underlying IByteTransport; ITransact concretes typically just
    /// delegate without wire-format processing.
    virtual Result<void> write(const uint8_t* data, size_t len) = 0;
    virtual Result<size_t> read(uint8_t* buf, size_t max, uint32_t timeout_ms) = 0;
};

}} // namespace note::spike
