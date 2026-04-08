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
#include <note/lexer/parse.hpp>
#include <note/transport_hal.hpp>
#include <note/compiler.hpp>
#include <note/types.hpp>

#include <algorithm>
#include <cstring>
#ifndef NOTE_MINIMAL
#include <string>
#endif

#ifndef NOTE_NO_CRC
#include <note/transport/detail/crc_types.hpp>
#endif

namespace note {

// ---------------------------------------------------------------------------
// NcErrorCapture — captures the Notecard "err" JSON field during parsing.
// Used by transact_dispatch to report Notecard errors without templated sinks.
// ---------------------------------------------------------------------------

namespace detail {

struct NcErrorCapture {
    static constexpr size_t kMaxLen = 64;
    char buf[kMaxLen]{};
    size_t len = 0;

    void capture(string_view v) {
        len = v.size() < kMaxLen ? v.size() : kMaxLen;
        for (size_t i = 0; i < len; ++i) buf[i] = v[i];
    }

    bool empty() const { return len == 0; }
    string_view view() const { return {buf, len}; }
};

/// ReceiveContext — wraps a SaxDispatch to intercept "err" and "crc" fields.
/// Creates a single wrapping dispatch table (not templated) so that
/// receive_dispatch can be a non-template member function.
struct ReceiveContext {
    SaxDispatch inner;
    NcErrorCapture& err;
#ifndef NOTE_NO_CRC
    uint16_t crc_seq = 0;
    uint32_t crc_checksum = 0;
    bool crc_found = false;
#endif

    ReceiveContext(SaxDispatch inner_, NcErrorCapture& err_)
        : inner(inner_), err(err_) {}

    /// Build a SaxDispatch that intercepts "err" and "crc" on_string events,
    /// forwarding everything else to the inner dispatch.
    SaxDispatch wrapping_dispatch() {
        SaxDispatch d;
        d.sink = this;
        d.on_null = [](void* p, string_view k) {
            auto& c = *static_cast<ReceiveContext*>(p);
            c.inner.on_null(c.inner.sink, k);
        };
        d.on_bool = [](void* p, string_view k, bool v) {
            auto& c = *static_cast<ReceiveContext*>(p);
            c.inner.on_bool(c.inner.sink, k, v);
        };
        d.on_int = [](void* p, string_view k, int32_t v) {
            auto& c = *static_cast<ReceiveContext*>(p);
            c.inner.on_int(c.inner.sink, k, v);
        };
        d.on_float = [](void* p, string_view k, double v) {
            auto& c = *static_cast<ReceiveContext*>(p);
            c.inner.on_float(c.inner.sink, k, v);
        };
        d.on_string = [](void* p, string_view k, string_view v) {
            auto& c = *static_cast<ReceiveContext*>(p);
            if (k == "err") {
                c.err.capture(v);
                // Also forward — virtual path's ErrorCaptureSink may need it.
                c.inner.on_string(c.inner.sink, k, v);
                return;
            }
#ifndef NOTE_NO_CRC
            if (k == "crc") {
                if (v.size() == 13 && v[4] == ':') {
                    c.crc_seq = static_cast<uint16_t>(
                        transport::detail::read_hex(v.data(), 4));
                    c.crc_checksum = static_cast<uint32_t>(
                        transport::detail::read_hex(v.data() + 5, 8));
                    c.crc_found = true;
                }
                return;  // CRC is transport-only, don't forward
            }
#endif
            c.inner.on_string(c.inner.sink, k, v);
        };
        d.on_object_begin = [](void* p, string_view k) {
            auto& c = *static_cast<ReceiveContext*>(p);
            c.inner.on_object_begin(c.inner.sink, k);
        };
        d.on_object_end = [](void* p, string_view k) {
            auto& c = *static_cast<ReceiveContext*>(p);
            c.inner.on_object_end(c.inner.sink, k);
        };
        d.on_array_begin = [](void* p, string_view k) {
            auto& c = *static_cast<ReceiveContext*>(p);
            c.inner.on_array_begin(c.inner.sink, k);
        };
        d.on_array_end = [](void* p, string_view k) {
            auto& c = *static_cast<ReceiveContext*>(p);
            c.inner.on_array_end(c.inner.sink, k);
        };
        d.reset = [](void* p) {
            auto& c = *static_cast<ReceiveContext*>(p);
            c.err = {};
#ifndef NOTE_NO_CRC
            c.crc_seq = 0;
            c.crc_checksum = 0;
            c.crc_found = false;
#endif
            c.inner.reset(c.inner.sink);
        };
        return d;
    }
};

} // namespace detail

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

#ifdef NOTE_MINIMAL
class StreamingTransport {
#else
class StreamingTransport : public IStreamingTransport {
#endif
public:
    explicit StreamingTransport(TransportHal& hal)
        : hal_(hal) {}

    /// @deprecated Retry is now orchestrated by the Notecard layer.
    /// These parameters are accepted for source compatibility but ignored.
    StreamingTransport(TransportHal& hal, uint32_t /*max_retries*/, uint32_t /*retry_delay_ms*/ = 500)
        : hal_(hal) {}

#if NOTE_DEBUG_ENABLED
#ifdef NOTE_MINIMAL
    void set_debug(const DebugListener& d) { debug_ = d; }
#else
    void set_debug(const DebugListener& d) override { debug_ = d; }
#endif
#endif

#ifdef NOTE_MINIMAL
    uint32_t millis() { return hal_.millis(); }
    void delay(uint32_t ms) { hal_.delay(ms); }
#else
    uint32_t millis() override { return hal_.millis(); }
    void delay(uint32_t ms) override { hal_.delay(ms); }

    /// Virtual override for IStreamingTransport (used by Notecard).
    Result<void> transact(BuildFn build_fn, void* ctx,
                          JsonSink& sink, uint32_t timeout_ms) override {
        return transact_impl(build_fn, ctx, sink, timeout_ms);
    }
#endif

    /// Template transact — concrete sink type, no vtable for sink dispatch.
    /// Used by StaticNotecard for zero-vtable execute.
    template<typename SinkT>
    Result<void> transact(BuildFn build_fn, void* ctx,
                          SinkT& sink, uint32_t timeout_ms) {
        return transact_impl(build_fn, ctx, sink, timeout_ms);
    }

    /// Non-template transact via SaxDispatch — single instantiation for all
    /// sink types. Error capture ("err") and CRC handling are done at the
    /// dispatch level, so the caller doesn't need ErrorCaptureSinkT/CrcFieldSinkT.
    ///
    /// @param dispatch  Type-erased sink dispatch table (from make_sax_dispatch).
    /// @param nc_err    Receives the Notecard "err" field content, if any.
    ///                  Valid as long as nc_err is in scope.
    Result<void> transact_dispatch(BuildFn build_fn, void* ctx,
                                   SaxDispatch dispatch, uint32_t timeout_ms,
                                   detail::NcErrorCapture& nc_err) {
        if (!ensure_init()) {
            debug_transport(debug_, TransportEvent::ResetFailed, 0);
            return make_error(Error::NotReady, NOTE_ERR("not ready"));
        }

#ifndef NOTE_NO_CRC
        if (crc_enabled_) ++crc_seq_;
#endif

        debug_timing(debug_, TimingEvent::TransmitBegin);
        if (!stream_request(build_fn, ctx)) {
            debug_transport(debug_, TransportEvent::SendFailed, 0);
            return make_error(Error::SendFailed, Cause::HalError, NOTE_ERR("transmit failed"));
        }
        debug_timing(debug_, TimingEvent::TransmitEnd);

        debug_timing(debug_, TimingEvent::ReceiveBegin);
        auto rv = receive_dispatch(dispatch, timeout_ms, nc_err);
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

private:
    /// Single-attempt transact. Retry is orchestrated by the Notecard layer.
    template<typename SinkT>
    Result<void> transact_impl(BuildFn build_fn, void* ctx,
                                SinkT& sink, uint32_t timeout_ms) {
        if (!ensure_init()) {
            debug_transport(debug_, TransportEvent::ResetFailed, 0);
            return make_error(Error::NotReady, NOTE_ERR("not ready"));
        }

#ifndef NOTE_NO_CRC
        if (crc_enabled_) ++crc_seq_;
#endif

        debug_timing(debug_, TimingEvent::TransmitBegin);
        if (!stream_request(build_fn, ctx)) {
            debug_transport(debug_, TransportEvent::SendFailed, 0);
            return make_error(Error::SendFailed, Cause::HalError, NOTE_ERR("transmit failed"));
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

    Result<void> send(BuildFn build_fn, void* ctx)
#ifndef NOTE_MINIMAL
        override
#endif
    {
        if (!ensure_init())
            return make_error(Error::NotReady, NOTE_ERR("not ready"));

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
            return make_error(Error::NotReady, NOTE_ERR("not ready"));

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
            return make_error(Error::NotReady, NOTE_ERR("not ready"));

        if (!hal_.transmit(reinterpret_cast<const uint8_t*>(json.data()), json.size()))
            return make_error(Error::SendFailed, Cause::HalError, NOTE_ERR("transmit failed"));
        if (!hal_.write_line_terminator())
            return make_error(Error::SendFailed, Cause::HalError, NOTE_ERR("line terminator failed"));
        return {};
    }

    void reset()
#ifndef NOTE_MINIMAL
        override
#endif
    {
        hal_.reset();
        initialized_ = false;
    }

    void abort()
#ifndef NOTE_MINIMAL
        override
#endif
    {}

    Result<void> write(const uint8_t* data, size_t len)
#ifndef NOTE_MINIMAL
        override
#endif
    {
        if (!hal_.transmit(data, len))
            return make_error(Error::SendFailed, Cause::HalError, NOTE_ERR("binary transmit failed"));
        return {};
    }

    Result<size_t> read(uint8_t* buf, size_t max_len, uint32_t timeout_ms)
#ifndef NOTE_MINIMAL
        override
#endif
    {
#ifndef NOTE_MINIMAL
        // Return any lookahead bytes saved by frame_read before hitting the HAL.
        if (lookahead_len_ > 0) {
            size_t n = std::min(max_len, lookahead_len_);
            memcpy(buf, lookahead_ + lookahead_pos_, n);
            lookahead_pos_ += n;
            lookahead_len_ -= n;
            return n;
        }
#endif
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
        auto dispatch = make_sax_dispatch(sink);
        detail::NcErrorCapture dummy_err;
        return receive_dispatch(dispatch, timeout_ms, dummy_err);
    }

    /// Non-template receive using SaxDispatch. Error capture ("err") and CRC
    /// interception are done via ReceiveContext wrapping at the dispatch level.
    /// This is the single implementation — both template and virtual paths delegate here.
    Result<void> receive_dispatch(SaxDispatch dispatch, uint32_t timeout_ms,
                                  detail::NcErrorCapture& nc_err) {
        detail::ReceiveContext ctx(dispatch, nc_err);
        auto wrapped = ctx.wrapping_dispatch();

        bool frame_terminated = false;
        bool any_data_received = false;

#ifndef NOTE_MINIMAL
        // Wire debug: accumulate response bytes when a listener is active.
        // Only allocates when debug_.on_wire is set — zero cost otherwise.
        std::string debug_recv;
#endif

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
    #ifndef NOTE_MINIMAL
                if (debug_.on_wire)
                        debug_recv.append(reinterpret_cast<const char*>(buf), i);
#endif
#ifndef NOTE_MINIMAL
                    // Save bytes after \n for subsequent read() calls
                    // (e.g. binary COBS data that arrived with the JSON response).
                    size_t after = n - i - 1;
                    if (after > 0 && after <= sizeof(lookahead_)) {
                        memcpy(lookahead_, buf + i + 1, after);
                        lookahead_pos_ = 0;
                        lookahead_len_ = after;
                    }
#endif
                    return i;  // bytes before \n only
                }
            }
#ifndef NOTE_MINIMAL
            if (debug_.on_wire)
                debug_recv.append(reinterpret_cast<const char*>(buf), n);
#endif
            return n;
        };

#ifndef NOTE_NO_CRC
        transport::detail::CrcAccumulator crc;

        auto read_fn = [&](uint8_t* buf, size_t max, uint32_t t) -> Result<size_t> {
            auto r = frame_read(buf, max, t);
            if (r) crc.feed(reinterpret_cast<const char*>(buf), *r);
            return r;
        };

        auto parse_err = sax_lex_streaming(read_fn, timeout_ms, wrapped);

        if (!frame_terminated && any_data_received)
            drain_frame_boundary();

        if (!parse_err.empty())
            return make_error(Error::ResponseLost, Cause::Unspecified, parse_err);

        if (ctx.crc_found) {
            if (ctx.crc_seq != crc_seq_ ||
                ctx.crc_checksum != crc.finalize_with_brace()) {
                return make_error(Error::ResponseLost, Cause::CrcMismatch, NOTE_ERR("CRC mismatch"));
            }
            crc_enabled_ = true;
        } else if (crc_enabled_) {
            return make_error(Error::ResponseLost, Cause::CrcMismatch, NOTE_ERR("expected CRC"));
        }
#else
        auto parse_err = sax_lex_streaming(frame_read, timeout_ms, wrapped);

        if (!frame_terminated && any_data_received)
            drain_frame_boundary();

        if (!parse_err.empty())
            return make_error(Error::ResponseLost, Cause::Unspecified, parse_err);
#endif
#ifndef NOTE_MINIMAL
        // Emit wire receive debug event with the accumulated response bytes.
        if (debug_.on_wire && !debug_recv.empty())
            debug_wire(debug_, string_view(debug_recv.data(), debug_recv.size()),
                       WireDirection::Receive);
#endif
        return {};
    }

    TransportHal& hal_;
    bool initialized_ = false;
#if NOTE_DEBUG_ENABLED
    DebugListener debug_{};
#else
    NoDebug debug_{};
#endif

#ifndef NOTE_MINIMAL
    // Lookahead buffer: bytes read from the HAL that arrived after \n
    // in the same chunk. Returned by subsequent read() calls (e.g. binary I/O).
    uint8_t lookahead_[64]{};
    size_t lookahead_pos_ = 0;
    size_t lookahead_len_ = 0;
#endif

#ifndef NOTE_NO_CRC
    bool crc_enabled_ = false;
    uint16_t crc_seq_ = 0;
#endif
};

} // namespace note
