#pragma once

/// @file spike/templated.hpp
/// SPIKE: templated, no-vtable variants of HalByteTransport + JsonTransact
/// for the StaticNotecard / AVR path. Same logic as the polymorphic
/// versions in hal_byte_transport.hpp / json_transact.hpp, just without
/// the virtual base classes — letting the compiler inline through both
/// layers for zero-overhead composition.

#include <note/error.hpp>
#include <note/json.hpp>
#include <note/json_sax.hpp>
#include <note/lexer/parse.hpp>
#include <note/protocol.hpp>     // for detail::NcErrorCapture / ReceiveContext
#include <note/request_source.hpp>
#include <note/span.hpp>
#include <note/transport_hal.hpp>
#include <note/types.hpp>

#if !NOTE_NO_CRC
#include <note/link/detail/crc_types.hpp>
#endif

#if NOTE_JSONB
#include <note/jsonb.hpp>
#endif

#include <algorithm>
#include <cstring>

namespace note { namespace spike {

/// Templated byte transport over any Hal-shaped type. No vtable on this
/// class — but it still calls `hal_.transmit/read/...` virtually if
/// `HalT` is `note::Hal`; for static use `HalT` should be a concrete
/// templated Hal (e.g. SerialFramer<SerialHal<HardwareSerial>>) so the
/// HAL calls also inline.
template<typename HalT>
class HalByteTransportT {
public:
    explicit HalByteTransportT(HalT& hal) : hal_(hal) {}

    Result<void> begin_transaction(uint32_t) {
        if (!ensure_init())
            return make_error(Error::NotReady, NOTE_ERR("not ready"));
        frame_terminated_ = false;
        any_data_received_ = false;
        return {};
    }

    void end_transaction() {
        if (!frame_terminated_ && any_data_received_)
            drain_frame_boundary();
    }

    Result<void> write(const uint8_t* data, size_t len) {
        if (!hal_.transmit(data, len))
            return make_error(Error::SendFailed, Cause::HalError,
                              NOTE_ERR("transmit failed"));
        return {};
    }

    Result<void> write_frame_terminator() {
        if (!hal_.write_line_terminator())
            return make_error(Error::SendFailed, Cause::HalError,
                              NOTE_ERR("line terminator failed"));
        return {};
    }

    Result<size_t> read(uint8_t* buf, size_t max, uint32_t timeout_ms) {
        if (frame_terminated_)
            return make_error(Error::EndOfFrame, NOTE_ERR("frame complete"));
        size_t n = 0;
        uint32_t start = hal_.millis();
        while (n == 0) {
            auto r = read_hal(buf, max, timeout_ms);
            if (!r) return r;
            n = *r;
            if (n == 0) {
                if (timeout_ms > 0 && hal_.millis() - start >= timeout_ms)
                    return make_error(Error::ResponseLost, Cause::Timeout,
                                      NOTE_ERR("timeout"));
                hal_.delay(1);
            }
        }
        any_data_received_ = true;
        for (size_t i = 0; i < n; ++i) {
            if (buf[i] == '\n') {
                frame_terminated_ = true;
#if !NOTE_NO_POLYMORPHIC
                // Lookahead is only needed when binary I/O can interleave
                // with JSON responses (the polymorphic Notecard supports
                // card.binary; minimal AVR builds don't).
                size_t after = n - i - 1;
                if (after > 0 && after <= sizeof(lookahead_)) {
                    std::memcpy(lookahead_, buf + i + 1, after);
                    lookahead_pos_ = 0;
                    lookahead_len_ = after;
                }
#endif
                return i;
            }
        }
        return n;
    }

    void reset() { hal_.reset(); initialized_ = false; }
    void abort() {}
    HalT& hal() { return hal_; }

private:
    Result<size_t> read_hal(uint8_t* buf, size_t max, uint32_t timeout_ms) {
#if !NOTE_NO_POLYMORPHIC
        if (lookahead_len_ > 0) {
            size_t n = std::min(max, lookahead_len_);
            std::memcpy(buf, lookahead_ + lookahead_pos_, n);
            lookahead_pos_ += n;
            lookahead_len_ -= n;
            return n;
        }
#endif
        return hal_.read(buf, max, timeout_ms);
    }

    bool ensure_init() {
        if (initialized_) return true;
        if (!hal_.reset()) return false;
        initialized_ = true;
        return true;
    }

    void drain_frame_boundary() {
        uint8_t byte;
        for (size_t guard = 0; guard < 256; ++guard) {
            auto rv = read_hal(&byte, 1, 250);
            if (!rv || *rv == 0) break;
            if (byte == '\n') break;
        }
    }

    HalT& hal_;
    bool initialized_ = false;
    bool frame_terminated_ = false;
    bool any_data_received_ = false;
#if !NOTE_NO_POLYMORPHIC
    uint8_t lookahead_[64]{};
    size_t lookahead_pos_ = 0;
    size_t lookahead_len_ = 0;
#endif
};

/// Templated JsonTransact wrapping any HalByteTransportT-shaped type.
/// No vtable; methods inline through the byte transport's methods.
template<typename ByteT>
class JsonTransactT {
public:
    explicit JsonTransactT(ByteT& byte_tx) : byte_(byte_tx) {}

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
#else
        auto parse_err = sax_lex_streaming(frame_read, timeout_ms, wrapped);
#endif
        byte_.end_transaction();
        if (!parse_err.empty())
            return make_error(Error::ResponseLost, Cause::Unspecified, parse_err);
        return {};
    }

private:
    Result<void> emit_(RequestSource src) {
        struct WireWriter : JsonWriter {
            using JsonWriter::write;
            ByteT& byte;
            bool ok = true;
            explicit WireWriter(ByteT& b) : byte(b) {}
            bool write(const char* data, size_t len) override {
                if (!ok) return false;
                auto r = byte.write(reinterpret_cast<const uint8_t*>(data), len);
                ok = r.has_value();
                return ok;
            }
        } wire(byte_);

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

            char suffix[link::detail::kCrcFieldLen + 1];
            size_t pos = 0;
            suffix[pos++] = ',';
            suffix[pos++] = '"'; suffix[pos++] = 'c'; suffix[pos++] = 'r';
            suffix[pos++] = 'c'; suffix[pos++] = '"'; suffix[pos++] = ':';
            suffix[pos++] = '"';
            link::detail::write_hex16(suffix + pos, crc_seq_); pos += 4;
            suffix[pos++] = ':';
            link::detail::write_hex32(suffix + pos, checksum); pos += 8;
            suffix[pos++] = '"';
            suffix[pos++] = '}';
            wire.write(suffix, pos);
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
#if !NOTE_NO_CRC
    uint16_t crc_seq_ = 0;
#endif
};

}} // namespace note::spike
