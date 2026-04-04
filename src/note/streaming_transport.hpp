#pragma once

/// @file streaming_transport.hpp
/// IStreamingTransport — interface for streaming Notecard communication.
/// StreamingTransport — protocol logic over a TransportHal.
///
/// Zero transport-internal buffers: requests are streamed via
/// StreamingJsonBuilder, responses SAX-parsed directly from the wire.

#include <note/debug.hpp>
#include <note/error.hpp>
#include <note/json.hpp>
#include <note/json_sax_streaming.hpp>
#include <note/transport_hal.hpp>
#include <note/compiler.hpp>
#include <note/types.hpp>

#include <algorithm>
#include <cstring>
#include <string>

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

    /// Monotonic millisecond counter for inter-transaction timing.
    virtual uint32_t millis() = 0;

    /// Platform delay.
    virtual void delay(uint32_t ms) = 0;

    /// Set debug listener. Default: no-op.
    virtual void set_debug(const DebugListener&) {}

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
    explicit StreamingTransport(TransportHal& hal)
        : hal_(hal) {}

    /// @deprecated Retry is now orchestrated by the Notecard layer.
    /// These parameters are accepted for source compatibility but ignored.
    StreamingTransport(TransportHal& hal, uint32_t /*max_retries*/, uint32_t /*retry_delay_ms*/ = 500)
        : hal_(hal) {}

    void set_debug(const DebugListener& d) override { debug_ = d; }

    uint32_t millis() override { return hal_.millis(); }
    void delay(uint32_t ms) override { hal_.delay(ms); }

    /// Virtual override for IStreamingTransport (used by Notecard).
    Result<void> transact(BuildFn build_fn, void* ctx,
                          JsonSink& sink, uint32_t timeout_ms) override {
        return transact_impl(build_fn, ctx, sink, timeout_ms);
    }

    /// Template transact — concrete sink type, no vtable for sink dispatch.
    /// Used by StaticNotecard for zero-vtable execute.
    template<typename SinkT>
    Result<void> transact(BuildFn build_fn, void* ctx,
                          SinkT& sink, uint32_t timeout_ms) {
        return transact_impl(build_fn, ctx, sink, timeout_ms);
    }

private:
    /// Single-attempt transact. Retry is orchestrated by the Notecard layer.
    template<typename SinkT>
    Result<void> transact_impl(BuildFn build_fn, void* ctx,
                                SinkT& sink, uint32_t timeout_ms) {
        if (!ensure_init()) {
            debug_transport(debug_, TransportEvent::ResetFailed, 0);
            return make_error(Error::NotReady, "Notecard not ready after reset");
        }

#ifndef NOTE_NO_CRC
        if (crc_enabled_) ++crc_seq_;
#endif

        debug_timing(debug_, TimingEvent::TransmitBegin);
        if (!stream_request(build_fn, ctx)) {
            debug_transport(debug_, TransportEvent::SendFailed, 0);
            return make_error(Error::SendFailed, Cause::HalError, "transmit failed");
        }
        debug_timing(debug_, TimingEvent::TransmitEnd);

        debug_timing(debug_, TimingEvent::ReceiveBegin);
        auto rv = receive_streaming(sink, timeout_ms);
        debug_timing(debug_, TimingEvent::ReceiveEnd);
        if (!rv) {
            if (rv.error().cause == Cause::Timeout)
                debug_transport(debug_, TransportEvent::Timeout, 0);
            else if (rv.error().cause == Cause::CrcMismatch)
                debug_transport(debug_, TransportEvent::CrcMismatch, 0);
            return Unexpected(rv.error());
        }
        return {};
    }

public:

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

    /// Raw passthrough: transmit pre-formatted JSON + line terminator,
    /// read response line into caller's buffer. No SAX parsing — raw bytes.
    Result<string_view> transact_raw(string_view json, char* buf, size_t bufsize,
                                      uint32_t timeout_ms) {
        if (!ensure_init())
            return make_error(Error::NotReady, "Notecard not ready after reset");

        // Transmit the raw JSON + line terminator
        if (!hal_.transmit(reinterpret_cast<const uint8_t*>(json.data()), json.size()))
            return make_error(Error::SendFailed, Cause::HalError, NOTE_ERR("transmit failed"));
        if (!hal_.write_line_terminator())
            return make_error(Error::SendFailed, Cause::HalError, NOTE_ERR("line terminator failed"));

        // Read response bytes until newline or timeout
        return read_line(buf, bufsize, timeout_ms);
    }

    /// Raw passthrough: transmit pre-formatted JSON + line terminator, no response.
    Result<void> send_raw(string_view json) {
        if (!ensure_init())
            return make_error(Error::NotReady, "Notecard not ready after reset");

        if (!hal_.transmit(reinterpret_cast<const uint8_t*>(json.data()), json.size()))
            return make_error(Error::SendFailed, Cause::HalError, NOTE_ERR("transmit failed"));
        if (!hal_.write_line_terminator())
            return make_error(Error::SendFailed, Cause::HalError, NOTE_ERR("line terminator failed"));
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
        // Return any lookahead bytes saved by frame_read before hitting the HAL.
        if (lookahead_len_ > 0) {
            size_t n = std::min(max_len, lookahead_len_);
            memcpy(buf, lookahead_ + lookahead_pos_, n);
            lookahead_pos_ += n;
            lookahead_len_ -= n;
            return n;
        }
        return hal_.read(buf, max_len, timeout_ms);
    }

private:
    /// Read bytes from HAL until newline, filling buf. Returns string_view into buf.
    /// If the response exceeds bufsize, drains the remainder and returns an error.
    Result<string_view> read_line(char* buf, size_t bufsize, uint32_t timeout_ms) {
        size_t pos = 0;
        bool overflow = false;
        for (;;) {
            uint8_t byte;
            auto rv = hal_.read(&byte, 1, timeout_ms);
            if (!rv) return Unexpected(rv.error());
            if (*rv == 0) return make_error(Error::ResponseLost, Cause::Timeout, NOTE_ERR("response timeout"));
            if (byte == '\n') break;
            if (byte == '\r') continue;
            if (pos < bufsize - 1)
                buf[pos++] = static_cast<char>(byte);
            else
                overflow = true;  // keep reading to drain, but flag the overflow
        }
        buf[pos] = '\0';
        if (overflow)
            return make_error(Error::Overflow, NOTE_ERR("response exceeds buffer"));
        return string_view(buf, pos);
    }

    /// Drain remaining bytes through the \n frame delimiter.
    /// Called when the frame-aware read didn't encounter \n (e.g. serial
    /// latency delayed the terminator past the end of SAX parsing).
    /// Uses a short timeout — \n follows } closely on the wire.
    static constexpr uint32_t kDrainTimeoutMs = 250;

    void drain_frame_boundary() {
        uint8_t byte;
        for (;;) {
            auto rv = hal_.read(&byte, 1, kDrainTimeoutMs);
            if (!rv || *rv == 0) break;
            if (byte == '\n') break;
        }
    }

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

    /// Receive and SAX-parse — template on sink type for static dispatch.
    /// Uses a frame-aware read that stops at the \n boundary, so the SAX
    /// parser never sees past the current frame. After parsing, drains any
    /// remaining frame bytes so the wire is clean for the next transaction.
    template<typename SinkT>
    Result<void> receive_streaming(SinkT& sink, uint32_t timeout_ms) {
        bool frame_terminated = false;
        bool any_data_received = false;

        // Wire debug: accumulate response bytes when a listener is active.
        // Only allocates when debug_.on_wire is set — zero cost otherwise.
        std::string debug_recv;

        // Frame-aware read: blocks until data arrives, truncates at \n.
        // JSON never contains a literal \n byte, so \n always delimits a frame.
        auto frame_read = [&](uint8_t* buf, size_t max, uint32_t t) -> Result<size_t> {
            if (frame_terminated)
                return make_error(Error::ResponseLost, Cause::Unspecified,
                                  NOTE_ERR("frame ended"));

            // Block until at least one byte arrives. The HAL contract says
            // read() blocks and returns error on timeout, but third-party
            // HALs may return 0 for "no data yet" — retry with delay.
            size_t n = 0;
            uint32_t start = hal_.millis();
            while (n == 0) {
                auto r = hal_.read(buf, max, t);
                if (!r) return r;
                n = *r;
                if (n == 0) {
                    if (t > 0 && hal_.millis() - start >= t)
                        return make_error(Error::ResponseLost, Cause::Timeout,
                                          NOTE_ERR("timeout"));
                    hal_.delay(1);
                }
            }
            any_data_received = true;

            for (size_t i = 0; i < n; ++i) {
                if (buf[i] == '\n') {
                    frame_terminated = true;
                    if (debug_.on_wire)
                        debug_recv.append(reinterpret_cast<const char*>(buf), i);
                    // Save bytes after \n for subsequent read() calls
                    // (e.g. binary COBS data that arrived with the JSON response).
                    size_t after = n - i - 1;
                    if (after > 0 && after <= sizeof(lookahead_)) {
                        memcpy(lookahead_, buf + i + 1, after);
                        lookahead_pos_ = 0;
                        lookahead_len_ = after;
                    }
                    return i;  // bytes before \n only
                }
            }
            if (debug_.on_wire)
                debug_recv.append(reinterpret_cast<const char*>(buf), n);
            return n;
        };

#ifndef NOTE_NO_CRC
        CrcFieldSinkT<SinkT> crc_sink(sink);
        transport::detail::CrcAccumulator crc;

        auto read_fn = [&](uint8_t* buf, size_t max, uint32_t t) -> Result<size_t> {
            auto r = frame_read(buf, max, t);
            if (r) crc.feed(reinterpret_cast<const char*>(buf), *r);
            return r;
        };

        auto parse_err = sax_parse_streaming(read_fn, timeout_ms, crc_sink);

        // Drain through \n if partial data was received but the frame
        // delimiter wasn't found. Skip drain when no data arrived at all
        // (e.g. timeout on first read) — there's nothing on the wire.
        if (!frame_terminated && any_data_received)
            drain_frame_boundary();

        if (!parse_err.empty())
            return make_error(Error::ResponseLost, Cause::Unspecified, parse_err);

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
        auto parse_err = sax_parse_streaming(frame_read, timeout_ms, sink);

        if (!frame_terminated && any_data_received)
            drain_frame_boundary();

        if (!parse_err.empty())
            return make_error(Error::ResponseLost, Cause::Unspecified, parse_err);
#endif
        // Emit wire receive debug event with the accumulated response bytes.
        if (debug_.on_wire && !debug_recv.empty())
            debug_wire(debug_, string_view(debug_recv.data(), debug_recv.size()),
                       WireDirection::Receive);
        return {};
    }

    TransportHal& hal_;
    bool initialized_ = false;
    DebugListener debug_{};

    // Lookahead buffer: bytes read from the HAL that arrived after \n
    // in the same chunk. Returned by subsequent read() calls (e.g. binary I/O).
    uint8_t lookahead_[64]{};
    size_t lookahead_pos_ = 0;
    size_t lookahead_len_ = 0;

#ifndef NOTE_NO_CRC
    bool crc_enabled_ = false;
    uint16_t crc_seq_ = 0;
#endif
};

} // namespace note
