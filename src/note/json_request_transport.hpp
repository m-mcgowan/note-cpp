#pragma once

/// @file json_transact.hpp
/// Concrete `ITransact` for JSON wire format. Wraps an `IByteTransport`,
/// owns CRC encoding + verification, JSONB COBS framing, and SAX-streaming
/// parser dispatch. Two flavors:
///
///   JsonRequestTransport      — polymorphic, inherits the wide `ITransact`. Used
///                       by the polymorphic Notecard / BareNotecard paths.
///
///   JsonRequestTransportT<ByteT> — templated, no vtable. Used by the static / AVR
///                          path (StaticNotecard); methods inline through
///                          the byte transport's methods so the compiler
///                          can fold both layers.
///
/// The polymorphic `JsonRequestTransport` carries the full ITransact surface so it
/// can stand in for `Protocol` at the SerialTransportStack level. The
/// templated `JsonRequestTransportT` carries the slimmer subset StaticNotecard
/// uses: `transact_dispatch(RequestSource, …)`, `send(RequestSource)`,
/// `transact_raw(string_view, …)`.

#include <note/error.hpp>
#include <note/json.hpp>
#include <note/json_sax.hpp>
#include <note/lexer/parse.hpp>
#include <note/protocol.hpp>  // for detail::NcErrorCapture / ReceiveContext
#include <note/request_source.hpp>
#include <note/span.hpp>
#include <note/transact.hpp>  // wide ITransact (polymorphic flavor inherits this)
#include <note/transport.hpp>
#include <note/transport_hal.hpp>
#include <note/types.hpp>

#if !NOTE_NO_CRC
#include <note/link/detail/crc_types.hpp>
#endif

#if NOTE_JSONB
#include <note/jsonb.hpp>
#endif

#include <cstring>

namespace note {

namespace detail {

/// CRC-trailer writer shared by `JsonRequestTransport` and `JsonRequestTransportT`.
/// Emits `,"crc":"SEQ:CHK"}` after the body. Caller supplies a writer
/// that already wraps the body in a `CrcWriter` and has called
/// `finalize_with_brace()` to obtain the checksum.
#if !NOTE_NO_CRC
inline void write_crc_trailer(JsonWriter& w, uint16_t seq, uint32_t checksum) {
    char suffix[link::detail::kCrcFieldLen + 1];
    size_t pos = 0;
    suffix[pos++] = ',';
    suffix[pos++] = '"'; suffix[pos++] = 'c'; suffix[pos++] = 'r';
    suffix[pos++] = 'c'; suffix[pos++] = '"'; suffix[pos++] = ':';
    suffix[pos++] = '"';
    link::detail::write_hex16(suffix + pos, seq); pos += 4;
    suffix[pos++] = ':';
    link::detail::write_hex32(suffix + pos, checksum); pos += 8;
    suffix[pos++] = '"';
    suffix[pos++] = '}';
    w.write(suffix, pos);
}
#endif

/// Wire writer that forwards JsonWriter::write to a byte transport.
/// Templated so the polymorphic and templated transports share one body.
template<typename ByteT>
struct ByteTransportWireWriter : JsonWriter {
    using JsonWriter::write;
    ByteT& byte;
    bool ok = true;
    explicit ByteTransportWireWriter(ByteT& b) : byte(b) {}
    bool write(const char* data, size_t len) override {
        if (!ok) return false;
        auto r = byte.write(reinterpret_cast<const uint8_t*>(data), len);
        ok = r.has_value();
        return ok;
    }
};

} // namespace detail

// ---------------------------------------------------------------------------
// JsonRequestTransportT<ByteT> — templated, no-vtable JSON transact
// ---------------------------------------------------------------------------

/// Templated JsonRequestTransport wrapping any IByteTransport-shaped type. No
/// vtable; methods inline through the byte transport's methods.
template<typename ByteT>
class JsonRequestTransportT {
public:
    explicit JsonRequestTransportT(ByteT& byte_tx) : byte_(byte_tx) {}

    Result<void> send(RequestSource src) {
        auto br = byte_.begin_transaction(1000);
        if (!br) return Unexpected(br.error());
        auto er = emit_(src);
        byte_.end_transaction();
        return er;
    }

    /// Drop-in for Protocol::transact_dispatch. Takes a RequestSource and
    /// a pre-built SaxDispatch (sink-table) so StaticNotecard can drive
    /// this without templating on SinkT.
    Result<void> transact_dispatch(RequestSource src,
                                   SaxDispatch dispatch,
                                   uint32_t timeout_ms,
                                   detail::NcErrorCapture& nc_err) {
        detail::ReceiveContext ctx(dispatch, nc_err);
        auto wrapped = ctx.wrapping_dispatch();

        auto br = byte_.begin_transaction(timeout_ms);
        if (!br) return Unexpected(br.error());

        auto er = emit_(src);
        if (!er) { byte_.end_transaction(); return Unexpected(er.error()); }

        auto frame_read = [&](uint8_t* b, size_t m, uint32_t t) -> Result<size_t> {
            return byte_.read(b, m, t);
        };

#if NOTE_JSONB
        // JSONB response: read {: header, COBS-decode, parse JSONB opcodes.
        uint8_t header[2]{};
        size_t hdr_got = 0;
        while (hdr_got < 2) {
            auto r = frame_read(header + hdr_got, 2 - hdr_got, timeout_ms);
            if (!r) { byte_.end_transaction();
                return Unexpected(r.error()); }
            if (*r == 0) { byte_.end_transaction();
                return make_error(Error::ResponseLost, Cause::Timeout,
                                  NOTE_ERR("JSONB header timeout")); }
            hdr_got += *r;
        }
        if (header[0] != '{' || header[1] != ':') {
            byte_.end_transaction();
            return make_error(Error::ResponseLost, Cause::Unspecified,
                              NOTE_ERR("expected JSONB header {:"));
        }
        detail::CobsDecodingReader<decltype(frame_read)> cobs_reader(
            frame_read, timeout_ms);
        char jsonb_storage[128];
        SaxStreamBuf jsonb_buf(jsonb_storage);
        auto parse_err = jsonb_parse_streaming(
            cobs_reader, timeout_ms, jsonb_buf, wrapped);
        byte_.end_transaction();
        if (!parse_err.empty())
            return make_error(Error::ResponseLost, Cause::Unspecified, parse_err);
        return {};
#else
#if !NOTE_NO_CRC
        link::detail::CrcAccumulator crc;
        auto read_fn = [&](uint8_t* b, size_t m, uint32_t t) -> Result<size_t> {
            auto r = frame_read(b, m, t);
            if (r) crc.feed(reinterpret_cast<const char*>(b), *r);
            return r;
        };
        auto parse_err = sax_lex_streaming(read_fn, timeout_ms, wrapped);
        byte_.end_transaction();
        if (!parse_err.empty())
            return make_error(Error::ResponseLost, Cause::Unspecified, parse_err);
        if (ctx.crc_found) {
            if (ctx.crc_seq != crc_seq_ ||
                ctx.crc_checksum != crc.finalize_with_brace()) {
                return make_error(Error::ResponseLost, Cause::CrcMismatch,
                                  NOTE_ERR("CRC mismatch"));
            }
            crc_enabled_ = true;
        } else if (crc_enabled_) {
            return make_error(Error::ResponseLost, Cause::CrcMismatch,
                              NOTE_ERR("expected CRC"));
        }
        return {};
#else
        auto parse_err = sax_lex_streaming(frame_read, timeout_ms, wrapped);
        byte_.end_transaction();
        if (!parse_err.empty())
            return make_error(Error::ResponseLost, Cause::Unspecified, parse_err);
        return {};
#endif
#endif
    }

    /// Raw passthrough: transmit pre-formatted JSON + line terminator, read
    /// response line into caller's buffer. No SAX parsing — raw bytes.
    /// No CRC injection (matches `Protocol::transact_raw` semantics).
    Result<string_view> transact_raw(string_view json, char* buf, size_t bufsize,
                                     uint32_t timeout_ms) {
        auto br = byte_.begin_transaction(timeout_ms);
        if (!br) return Unexpected(br.error());

        auto wr = byte_.write(reinterpret_cast<const uint8_t*>(json.data()), json.size());
        if (!wr) { byte_.end_transaction(); return Unexpected(wr.error()); }
        auto wt = byte_.write_frame_terminator();
        if (!wt) { byte_.end_transaction(); return Unexpected(wt.error()); }

        size_t pos = 0;
        bool overflow = false;
        for (;;) {
            uint8_t byte;
            auto rv = byte_.read(&byte, 1, timeout_ms);
            if (!rv) {
                if (rv.error().code == Error::EndOfFrame) break;
                byte_.end_transaction();
                return Unexpected(rv.error());
            }
            if (*rv == 0) {
                byte_.end_transaction();
                return make_error(Error::ResponseLost, Cause::Timeout,
                                  NOTE_ERR("response timeout"));
            }
            if (byte == '\r') continue;
            if (pos < bufsize - 1)
                buf[pos++] = static_cast<char>(byte);
            else
                overflow = true;
        }
        byte_.end_transaction();
        buf[pos] = '\0';
        if (overflow)
            return make_error(Error::Overflow, NOTE_ERR(
                "response exceeds buffer; enlarge with nc.set_response_buffer() "
                "or wire .into(JsonSink&) for streaming"));
        return string_view(buf, pos);
    }

    void reset() { byte_.reset(); }
    void abort() { byte_.abort(); }
    auto& hal() { return byte_.hal(); }
    ByteT& byte() { return byte_; }

private:
    /// Paint the request fields through a CRC-tracking (or COBS-framing)
    /// writer over the byte transport. Forms the body, appends the
    /// `,"crc":"SEQ:CHK"}` trailer (JSON+CRC) or the JSONB end-object +
    /// `:}` envelope (JSONB), then writes the line terminator.
    Result<void> emit_(RequestSource src) {
        detail::ByteTransportWireWriter<ByteT> wire(byte_);

#if NOTE_JSONB
        wire.write("{:", 2);
        CobsStreamWriter cobs(wire, jsonb::kCobsXor);
        src.emit(cobs);
        cobs.write(reinterpret_cast<const char*>(&jsonb::kEndObject), 1);
        cobs.flush();
        wire.write(":}", 2);
#elif !NOTE_NO_CRC
        ++crc_seq_;
        {
            CrcWriter crc(wire);
            src.emit(crc);
            uint32_t checksum = crc.finalize_with_brace();
            detail::write_crc_trailer(wire, crc_seq_, checksum);
        }
#else
        src.emit(wire);
        wire.write("}", 1);
#endif

        if (!wire.ok)
            return make_error(Error::SendFailed, Cause::HalError,
                              NOTE_ERR("transmit failed"));
        return byte_.write_frame_terminator();
    }

    ByteT& byte_;
#if !NOTE_NO_CRC && !NOTE_JSONB
    uint16_t crc_seq_ = 0;
    bool crc_enabled_ = false;
#endif
};

#if !NOTE_NO_POLYMORPHIC && !NOTE_STATIC_HAL

// ---------------------------------------------------------------------------
// JsonRequestTransport — polymorphic, inherits the wide `ITransact`
// ---------------------------------------------------------------------------

/// Concrete `ITransact` for JSON wire format over an `IByteTransport&`.
/// Carries the full ITransact surface (string_view + RequestSource shapes,
/// transact + send) so it can stand in for `Protocol` at the
/// `SerialTransportStack` level.
class JsonRequestTransport : public ITransact {
public:
    explicit JsonRequestTransport(IByteTransport& byte_tx) : byte_(byte_tx) {}

    // ─── RequestSource shape — native implementations ───────────────────

    Result<void> transact(RequestSource src, JsonSink& sink,
                          uint32_t timeout_ms) override {
        auto dispatch = make_sax_dispatch(sink);
        detail::NcErrorCapture nc_err;
        return transact_dispatch_impl(src, dispatch, timeout_ms, nc_err);
    }

    Result<string_view> transact(RequestSource src, span<char> buf,
                                 uint32_t timeout_ms) override {
        auto br = byte_.begin_transaction(timeout_ms);
        if (!br) return Unexpected(br.error());

        auto er = emit_(src);
        if (!er) { byte_.end_transaction(); return Unexpected(er.error()); }

        auto rv = read_into_buf(buf, timeout_ms);
        byte_.end_transaction();
        return rv;
    }

    Result<void> send(RequestSource src) override {
        auto br = byte_.begin_transaction(1000);
        if (!br) return Unexpected(br.error());
        auto er = emit_(src);
        byte_.end_transaction();
        return er;
    }

    // ─── string_view shape — raw passthrough (no CRC injection) ─────────

    Result<string_view> transact(string_view request, span<char> buf,
                                 uint32_t timeout_ms) override {
        return transact_raw(request, buf.data(), buf.size(), timeout_ms);
    }

    Result<void> send(string_view request) override {
        auto br = byte_.begin_transaction(1000);
        if (!br) return Unexpected(br.error());
        auto wr = byte_.write(reinterpret_cast<const uint8_t*>(request.data()), request.size());
        if (!wr) { byte_.end_transaction(); return Unexpected(wr.error()); }
        auto wt = byte_.write_frame_terminator();
        byte_.end_transaction();
        return wt;
    }

    // Inherit default transact(string_view, JsonSink&, t) bridge from ITransact.
    using ITransact::transact;
    using ITransact::send;

    // ─── BuildFn / Protocol parity surface ──────────────────────────────

    /// Drop-in for `Protocol::transact_dispatch(RequestSource, …)`.
    /// Same semantics: error capture and CRC handling at the dispatch
    /// level via `ReceiveContext`. Non-virtual; used by tests and the
    /// migration shim path.
    Result<void> transact_dispatch(RequestSource src,
                                   SaxDispatch dispatch, uint32_t timeout_ms,
                                   detail::NcErrorCapture& nc_err) {
        return transact_dispatch_impl(src, dispatch, timeout_ms, nc_err);
    }

    /// Raw passthrough: transmit pre-formatted JSON + line terminator, read
    /// response into caller's buffer. No SAX parsing, no CRC injection.
    Result<string_view> transact_raw(string_view json, char* buf, size_t bufsize,
                                     uint32_t timeout_ms) {
        auto br = byte_.begin_transaction(timeout_ms);
        if (!br) return Unexpected(br.error());

        auto wr = byte_.write(reinterpret_cast<const uint8_t*>(json.data()), json.size());
        if (!wr) { byte_.end_transaction(); return Unexpected(wr.error()); }
        auto wt = byte_.write_frame_terminator();
        if (!wt) { byte_.end_transaction(); return Unexpected(wt.error()); }

        size_t pos = 0;
        bool overflow = false;
        for (;;) {
            uint8_t byte;
            auto rv = byte_.read(&byte, 1, timeout_ms);
            if (!rv) {
                if (rv.error().code == Error::EndOfFrame) break;
                byte_.end_transaction();
                return Unexpected(rv.error());
            }
            if (*rv == 0) {
                byte_.end_transaction();
                return make_error(Error::ResponseLost, Cause::Timeout,
                                  NOTE_ERR("response timeout"));
            }
            if (byte == '\r') continue;
            if (pos < bufsize - 1)
                buf[pos++] = static_cast<char>(byte);
            else
                overflow = true;
        }
        byte_.end_transaction();
        buf[pos] = '\0';
        if (overflow)
            return make_error(Error::Overflow, NOTE_ERR(
                "response exceeds buffer; enlarge with nc.set_response_buffer() "
                "or wire .into(JsonSink&) for streaming"));
        return string_view(buf, pos);
    }

    // ─── ITransact infrastructure ───────────────────────────────────────

    void reset() override { byte_.reset(); }
    void abort() override { byte_.abort(); }
    Hal& hal() override { return byte_.hal(); }
    void set_debug(const DebugListener& d) override { byte_.set_debug(d); }

    Result<void> write(const uint8_t* data, size_t len) override {
        return byte_.write(data, len);
    }
    Result<size_t> read(uint8_t* buf, size_t max, uint32_t timeout_ms) override {
        return byte_.read(buf, max, timeout_ms);
    }

    IByteTransport& byte() { return byte_; }

private:
    Result<string_view> read_into_buf(span<char> buf, uint32_t timeout_ms) {
        size_t pos = 0;
        bool overflow = false;
        for (;;) {
            auto r = byte_.read(reinterpret_cast<uint8_t*>(buf.data()) + pos,
                                buf.size() - pos, timeout_ms);
            if (!r) {
                if (r.error().code == Error::EndOfFrame) break;
                return Unexpected(r.error());
            }
            pos += *r;
            if (pos >= buf.size()) { overflow = true; break; }
        }
        if (overflow)
            return make_error(Error::Overflow, NOTE_ERR(
                "response exceeds buffer; enlarge with nc.set_response_buffer() "
                "or wire .into(JsonSink&) for streaming"));
        if (pos > 0 && buf.data()[pos - 1] == '\r') --pos;
        return string_view(buf.data(), pos);
    }

    Result<void> transact_dispatch_impl(RequestSource src,
                                        SaxDispatch dispatch, uint32_t timeout_ms,
                                        detail::NcErrorCapture& nc_err) {
        detail::ReceiveContext ctx(dispatch, nc_err);
        auto wrapped = ctx.wrapping_dispatch();

        auto br = byte_.begin_transaction(timeout_ms);
        if (!br) return Unexpected(br.error());

        auto er = emit_(src);
        if (!er) { byte_.end_transaction(); return Unexpected(er.error()); }

        auto frame_read = [&](uint8_t* b, size_t m, uint32_t t) -> Result<size_t> {
            return byte_.read(b, m, t);
        };

#if NOTE_JSONB
        uint8_t header[2]{};
        size_t hdr_got = 0;
        while (hdr_got < 2) {
            auto r = frame_read(header + hdr_got, 2 - hdr_got, timeout_ms);
            if (!r) { byte_.end_transaction(); return Unexpected(r.error()); }
            if (*r == 0) {
                byte_.end_transaction();
                return make_error(Error::ResponseLost, Cause::Timeout,
                                  NOTE_ERR("JSONB header timeout"));
            }
            hdr_got += *r;
        }
        if (header[0] != '{' || header[1] != ':') {
            byte_.end_transaction();
            return make_error(Error::ResponseLost, Cause::Unspecified,
                              NOTE_ERR("expected JSONB header {:"));
        }
        detail::CobsDecodingReader<decltype(frame_read)> cobs_reader(
            frame_read, timeout_ms);
        char jsonb_storage[128];
        SaxStreamBuf jsonb_buf(jsonb_storage);
        auto parse_err = jsonb_parse_streaming(
            cobs_reader, timeout_ms, jsonb_buf, wrapped);
        byte_.end_transaction();
        if (!parse_err.empty())
            return make_error(Error::ResponseLost, Cause::Unspecified, parse_err);
        return {};
#else
#if !NOTE_NO_CRC
        link::detail::CrcAccumulator crc;
        auto read_fn = [&](uint8_t* b, size_t m, uint32_t t) -> Result<size_t> {
            auto r = frame_read(b, m, t);
            if (r) crc.feed(reinterpret_cast<const char*>(b), *r);
            return r;
        };
        auto parse_err = sax_lex_streaming(read_fn, timeout_ms, wrapped);
        byte_.end_transaction();
        if (!parse_err.empty())
            return make_error(Error::ResponseLost, Cause::Unspecified, parse_err);
        if (ctx.crc_found) {
            if (ctx.crc_seq != crc_seq_ ||
                ctx.crc_checksum != crc.finalize_with_brace()) {
                return make_error(Error::ResponseLost, Cause::CrcMismatch,
                                  NOTE_ERR("CRC mismatch"));
            }
            crc_enabled_ = true;
        } else if (crc_enabled_) {
            return make_error(Error::ResponseLost, Cause::CrcMismatch,
                              NOTE_ERR("expected CRC"));
        }
        return {};
#else
        auto parse_err = sax_lex_streaming(frame_read, timeout_ms, wrapped);
        byte_.end_transaction();
        if (!parse_err.empty())
            return make_error(Error::ResponseLost, Cause::Unspecified, parse_err);
        return {};
#endif
#endif
    }

    Result<void> emit_(RequestSource src) {
        detail::ByteTransportWireWriter<IByteTransport> wire(byte_);

#if NOTE_JSONB
        wire.write("{:", 2);
        CobsStreamWriter cobs(wire, jsonb::kCobsXor);
        src.emit(cobs);
        cobs.write(reinterpret_cast<const char*>(&jsonb::kEndObject), 1);
        cobs.flush();
        wire.write(":}", 2);
#elif !NOTE_NO_CRC
        ++crc_seq_;
        {
            CrcWriter crc(wire);
            src.emit(crc);
            uint32_t checksum = crc.finalize_with_brace();
            detail::write_crc_trailer(wire, crc_seq_, checksum);
        }
#else
        src.emit(wire);
        wire.write("}", 1);
#endif

        if (!wire.ok)
            return make_error(Error::SendFailed, Cause::HalError,
                              NOTE_ERR("transmit failed"));
        return byte_.write_frame_terminator();
    }

    IByteTransport& byte_;
#if !NOTE_NO_CRC && !NOTE_JSONB
    uint16_t crc_seq_ = 0;
    bool crc_enabled_ = false;
#endif
};

#endif // !NOTE_NO_POLYMORPHIC && !NOTE_STATIC_HAL

} // namespace note
