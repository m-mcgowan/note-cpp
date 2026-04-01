#pragma once

/// @file streaming_transport.hpp
/// IStreamingTransport — interface for streaming Notecard communication.
/// StreamingTransport — protocol logic over a TransportHal.
///
/// Zero transport-internal buffers: requests are streamed via
/// StreamingJsonBuilder, responses SAX-parsed directly from the wire.

#include <note/error.hpp>
#include <note/json.hpp>
#include <note/json_sax_streaming.hpp>
#include <note/transport_hal.hpp>
#include <note/compiler.hpp>
#include <note/types.hpp>

#ifndef NOTE_NO_CRC
#include <note/transport/detail/crc_types.hpp>
#endif

namespace note {

// ---------------------------------------------------------------------------
// IStreamingTransport
// ---------------------------------------------------------------------------

/// Type-erased build function for streaming request construction.
using BuildFn = void(*)(JsonBuilder&, void*);

/// Streaming transport interface for Notecard communication.
struct IStreamingTransport {
    virtual ~IStreamingTransport() = default;

    /// Send a JSON request and SAX-parse the response into sink.
    virtual Result<void> transact(BuildFn build_fn, void* ctx,
                                  JsonSink& sink, uint32_t timeout_ms) = 0;

    /// Send a JSON command (fire-and-forget).
    virtual Result<void> send(BuildFn build_fn, void* ctx) = 0;

    virtual void reset() = 0;
    virtual void abort() = 0;

    /// Raw binary I/O (for COBS streaming). Default: not supported.
    virtual Result<void> write(const uint8_t*, size_t) {
        return make_error(Error::NotReady, NOTE_ERR("binary transfer not supported"));
    }
    virtual Result<size_t> read(uint8_t*, size_t, uint32_t) {
        return make_error(Error::NotReady, NOTE_ERR("binary transfer not supported"));
    }

    /// Convenience: type-erase a callable.
    template<typename F>
    Result<void> transact(F&& f, JsonSink& sink, uint32_t timeout_ms) {
        BuildFn fn = [](JsonBuilder& b, void* p) {
            (*static_cast<std::remove_reference_t<F>*>(p))(b);
        };
        return transact(fn, &f, sink, timeout_ms);
    }

    template<typename F>
    Result<void> send(F&& f) {
        BuildFn fn = [](JsonBuilder& b, void* p) {
            (*static_cast<std::remove_reference_t<F>*>(p))(b);
        };
        return send(fn, &f);
    }
};


// ---------------------------------------------------------------------------
// StreamingTransport — protocol logic over a TransportHal
// ---------------------------------------------------------------------------

class StreamingTransport : public IStreamingTransport {
public:
    explicit StreamingTransport(TransportHal& hal,
                                uint32_t max_retries = 5,
                                uint32_t retry_delay_ms = 500)
        : hal_(hal)
        , max_retries_(max_retries)
        , retry_delay_ms_(retry_delay_ms) {}

    Result<void> transact(BuildFn build_fn, void* ctx,
                          JsonSink& sink, uint32_t timeout_ms) override {
        if (!ensure_init())
            return make_error(Error::NotReady, "Notecard not ready after reset");

#ifndef NOTE_NO_CRC
        if (crc_enabled_) ++crc_seq_;
#endif

        ErrorInfo last_error{Error::SendFailed, Cause::HalError, "transmit failed"};

        for (uint32_t attempt = 0; attempt <= max_retries_; ++attempt) {
            if (attempt > 0) {
                hal_.delay(retry_delay_ms_);
                hal_.reset();
                sink.reset();
            }

            if (!stream_request(build_fn, ctx)) {
                last_error = {Error::SendFailed, Cause::HalError, "transmit failed"};
                continue;
            }

            auto rv = receive_streaming(sink, timeout_ms);
            if (!rv) {
                last_error = rv.error();
                continue;
            }
            return {};
        }

        return Unexpected(last_error);
    }

    Result<void> send(BuildFn build_fn, void* ctx) override {
        if (!ensure_init())
            return make_error(Error::NotReady, "Notecard not ready after reset");

#ifndef NOTE_NO_CRC
        if (crc_enabled_) ++crc_seq_;
#endif

        if (!stream_request(build_fn, ctx))
            return make_error(Error::SendFailed, Cause::HalError, NOTE_ERR("transmit failed"));
        return {};
    }

    void reset() override {
        hal_.reset();
        initialized_ = false;
    }

    void abort() override {}

    Result<void> write(const uint8_t* data, size_t len) override {
        if (!hal_.transmit(data, len))
            return make_error(Error::SendFailed, Cause::HalError, NOTE_ERR("binary transmit failed"));
        return {};
    }

    Result<size_t> read(uint8_t* buf, size_t max_len, uint32_t timeout_ms) override {
        return hal_.read(buf, max_len, timeout_ms);
    }

private:
    bool ensure_init() {
        if (initialized_) return true;
        if (!hal_.reset()) return false;
        initialized_ = true;
        return true;
    }

    bool stream_request(BuildFn build_fn, void* ctx) {
        struct Writer : JsonWriter {
            using JsonWriter::write;
            TransportHal& hal;
            bool ok = true;
            explicit Writer(TransportHal& h) : hal(h) {}
            bool write(const char* data, size_t len) override {
                if (!ok) return false;
                ok = hal.transmit(reinterpret_cast<const uint8_t*>(data), len);
                return ok;
            }
        } writer(hal_);

#ifndef NOTE_NO_CRC
        if (crc_enabled_) {
            CrcWriter crc(writer);
            StreamingJsonBuilder builder(crc);
            build_fn(builder, ctx);

            uint32_t checksum = crc.finalize_with_brace();

            char suffix[transport::detail::kCrcFieldLen + 1];
            size_t pos = 0;
            suffix[pos++] = ',';
            suffix[pos++] = '"'; suffix[pos++] = 'c'; suffix[pos++] = 'r';
            suffix[pos++] = 'c'; suffix[pos++] = '"'; suffix[pos++] = ':';
            suffix[pos++] = '"';
            transport::detail::write_hex16(suffix + pos, crc_seq_); pos += 4;
            suffix[pos++] = ':';
            transport::detail::write_hex32(suffix + pos, checksum); pos += 8;
            suffix[pos++] = '"';
            suffix[pos++] = '}';
            writer.write(suffix, pos);
        } else
#endif
        {
            StreamingJsonBuilder builder(writer);
            build_fn(builder, ctx);
            writer.write("}", 1);
        }

        if (!writer.ok) return false;
        return hal_.write_line_terminator();
    }

    Result<void> receive_streaming(JsonSink& sink, uint32_t timeout_ms) {
#ifndef NOTE_NO_CRC
        CrcFieldSink crc_sink(sink);
        transport::detail::CrcAccumulator crc;

        auto read_fn = [&](uint8_t* buf, size_t max, uint32_t t) -> Result<size_t> {
            auto r = hal_.read(buf, max, t);
            if (r) crc.feed(reinterpret_cast<const char*>(buf), *r);
            return r;
        };

        auto parse_err = sax_parse_streaming(read_fn, timeout_ms, crc_sink);
        if (!parse_err.empty())
            return make_error(Error::Json, parse_err);

        if (crc_sink.has_crc()) {
            if (crc_sink.seq() != crc_seq_ ||
                crc_sink.checksum() != crc.finalize_with_brace()) {
                return make_error(Error::ResponseLost, Cause::CrcMismatch, "CRC mismatch");
            }
            crc_enabled_ = true;
        } else if (crc_enabled_) {
            return make_error(Error::ResponseLost, Cause::CrcMismatch, NOTE_ERR("expected CRC"));
        }
#else
        auto read_fn = [&](uint8_t* buf, size_t max, uint32_t t) -> Result<size_t> {
            return hal_.read(buf, max, t);
        };

        auto parse_err = sax_parse_streaming(read_fn, timeout_ms, sink);
        if (!parse_err.empty())
            return make_error(Error::Json, parse_err);
#endif
        return {};
    }

    TransportHal& hal_;
    uint32_t max_retries_;
    uint32_t retry_delay_ms_;
    bool initialized_ = false;
#ifndef NOTE_NO_CRC
    bool crc_enabled_ = false;
    uint16_t crc_seq_ = 0;
#endif
};

} // namespace note
