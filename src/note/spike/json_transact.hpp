#pragma once

/// @file spike/json_transact.hpp
/// SPIKE: concrete ITransact for JSON wire format. Wraps an IByteTransport,
/// owns CRC encoding/verification, drives SAX-streaming parser for sink
/// mode and accumulates bytes into the caller's span for buffered mode.

#include <note/lexer/parse.hpp>
#include <note/json_sax.hpp>
#include <note/transport.hpp>

#if !NOTE_NO_CRC
#include <note/link/detail/crc_types.hpp>
#endif

namespace note { namespace spike {

class JsonTransact : public ITransact {
public:
    explicit JsonTransact(IByteTransport& byte_tx) : byte_(byte_tx) {}

    Result<void> transact(RequestSource src, JsonSink& sink,
                          uint32_t timeout_ms) override {
        auto br = byte_.begin_transaction(timeout_ms);
        if (!br) return Unexpected(br.error());

        auto er = emit_(src);
        if (!er) { byte_.end_transaction(); return Unexpected(er.error()); }

        auto read_fn = [&](uint8_t* b, size_t m, uint32_t t) -> Result<size_t> {
            return byte_.read(b, m, t);
        };
        auto parse_err = sax_lex_streaming(read_fn, timeout_ms, sink);
        byte_.end_transaction();
        if (!parse_err.empty())
            return make_error(Error::ResponseLost, Cause::Unspecified, parse_err);
        return {};
    }

    Result<string_view> transact(RequestSource src, span<char> buf,
                                  uint32_t timeout_ms) override {
        auto br = byte_.begin_transaction(timeout_ms);
        if (!br) return Unexpected(br.error());

        auto er = emit_(src);
        if (!er) { byte_.end_transaction(); return Unexpected(er.error()); }

        // Read bytes into the buffer until EndOfFrame.
        size_t pos = 0;
        bool overflow = false;
        for (;;) {
            auto r = byte_.read(reinterpret_cast<uint8_t*>(buf.data()) + pos,
                                buf.size() - pos, timeout_ms);
            if (!r) {
                if (r.error().code == Error::EndOfFrame) break;
                byte_.end_transaction();
                return Unexpected(r.error());
            }
            pos += *r;
            if (pos >= buf.size()) { overflow = true; break; }
        }
        byte_.end_transaction();
        if (overflow)
            return make_error(Error::Overflow, NOTE_ERR(
                "response exceeds buffer; enlarge with nc.set_response_buffer() "
                "or wire .into(JsonSink&) for streaming"));
        // Trim \r if present at end (Serial sends \r\n; we stop at \n which
        // is consumed by HalByteTransport, but \r may remain).
        if (pos > 0 && buf.data()[pos - 1] == '\r') --pos;
        return string_view(buf.data(), pos);
    }

    Result<void> send(RequestSource src) override {
        auto br = byte_.begin_transaction(/*timeout_ms*/1000);
        if (!br) return Unexpected(br.error());
        auto er = emit_(src);
        byte_.end_transaction();
        return er;
    }

    void reset() override { byte_.reset(); }
    void abort() override { byte_.abort(); }
    Hal& hal() override { return byte_.hal(); }

    Result<void> write(const uint8_t* d, size_t n) override {
        return byte_.write(d, n);
    }
    Result<size_t> read(uint8_t* b, size_t m, uint32_t t) override {
        return byte_.read(b, m, t);
    }

private:
    /// Paint the request fields through a CRC-tracking writer over the
    /// byte transport. The writer chunks per JsonWriter::write call; each
    /// chunk goes through byte_.write(). On finalise, write the
    /// `,"crc":"SEQ:CHK"}` trailer + line terminator.
    Result<void> emit_(RequestSource src) {
        struct WireWriter : JsonWriter {
            using JsonWriter::write;
            IByteTransport& byte;
            bool ok = true;
            explicit WireWriter(IByteTransport& b) : byte(b) {}
            bool write(const char* data, size_t len) override {
                if (!ok) return false;
                auto r = byte.write(reinterpret_cast<const uint8_t*>(data), len);
                ok = r.has_value();
                return ok;
            }
        } wire(byte_);

#if !NOTE_NO_CRC
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

    IByteTransport& byte_;
#if !NOTE_NO_CRC
    uint16_t crc_seq_ = 0;
#endif
};

}} // namespace note::spike
