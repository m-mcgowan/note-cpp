#pragma once

/// @file protocol.hpp
/// Protocol — Notecard wire-protocol driver over a `Hal`. Implements
/// the unified `ITransport` interface natively (both transact overloads
/// and `send`), and additionally exposes BuildFn-based virtuals via
/// `IStreamingTransport` for callers that want to stream the request
/// build directly to the wire (e.g. `StaticNotecard`).
///
/// IStreamingTransport — deprecated bridge: derives from ITransport,
/// adds the BuildFn virtuals. Existing test stubs that only override
/// the BuildFn shape continue to compile.
///
/// Zero transport-internal buffers: requests are streamed via
/// StreamingJsonBuilder, responses SAX-parsed directly from the wire.
///
/// Renamed from `StreamingTransport` in Phase 4 of the transport
/// refactor; `using StreamingTransport = Protocol` is kept for one
/// release as a deprecated alias.

#include <note/debug.hpp>
#include <note/error.hpp>
#include <note/json.hpp>
#include <note/json_sax_streaming.hpp>
#include <note/lexer/parse.hpp>
#include <note/transport.hpp>
#include <note/transport_hal.hpp>
#include <note/wire_format.hpp>
#include <note/compiler.hpp>
#include <note/types.hpp>

#if NOTE_JSONB
#include <note/jsonb.hpp>
#endif

#include <algorithm>
#include <cstring>
#if NOTE_DEBUG_ENABLED
#include <string>
#endif

#if !NOTE_NO_CRC
#include <note/transport/detail/crc_types.hpp>
#endif

#if NOTE_TXN_HANDSHAKE
#include <note/txn_handshake.hpp>
#endif

namespace note {

#if NOTE_TXN_HANDSHAKE
namespace detail {

/// RAII scope for an optional TxnHandshake. Calls start() on construct and
/// stop() on destruct. If no TxnHandshake is registered (null pointer) the
/// scope is a no-op and ok() is true. If start() returns false, ok() is
/// false and stop() is NOT called on destruct (nothing to release).
class TxnHandshakeScope {
public:
    TxnHandshakeScope(TxnHandshake* h, uint32_t timeout_ms) : handshake_(h) {
        if (handshake_) started_ = handshake_->start(timeout_ms);
    }
    ~TxnHandshakeScope() { if (handshake_ && started_) handshake_->stop(); }
    TxnHandshakeScope(const TxnHandshakeScope&) = delete;
    TxnHandshakeScope(TxnHandshakeScope&&) = delete;
    TxnHandshakeScope& operator=(const TxnHandshakeScope&) = delete;
    TxnHandshakeScope& operator=(TxnHandshakeScope&&) = delete;

    /// True if no handshake is needed (null handshake_) or start() succeeded.
    bool ok() const noexcept { return !handshake_ || started_; }

private:
    TxnHandshake* handshake_;
    bool started_ = false;
};

/// Default timeout for transaction-handshake start() when the caller
/// doesn't supply one (e.g. fire-and-forget send()). Deliberately generous
/// — the handshake is waiting on a physical wake signal from the Notecard.
inline constexpr uint32_t kTxnHandshakeDefaultTimeoutMs = 1000;

} // namespace detail
#endif // NOTE_TXN_HANDSHAKE

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
#if !NOTE_NO_CRC
    uint16_t crc_seq = 0;
    uint32_t crc_checksum = 0;
    bool crc_found = false;
#endif

    ReceiveContext(SaxDispatch inner_, NcErrorCapture& err_)
        : inner(inner_), err(err_) {}

    /// Build a SaxDispatch that intercepts "err" and "crc" on_string events,
    /// forwarding everything else to the inner dispatch.
    SaxDispatch wrapping_dispatch() {
        return SaxDispatch{
            this,
            [](void* p, const SaxEvent& ev) {
                auto& c = *static_cast<ReceiveContext*>(p);
                if (ev.tag == SaxEvent::String) {
                    auto v = string_view(ev.sv.data, ev.sv.len);
                    if (ev.key == "err") {
                        c.err.capture(v);
                        // Also forward — virtual path's ErrorCaptureSink may need it.
                        c.inner.dispatch(c.inner.sink, ev);
                        return;
                    }
#if !NOTE_NO_CRC
                    if (ev.key == "crc") {
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
                }
                if (ev.tag == SaxEvent::Reset) {
                    c.err = {};
#if !NOTE_NO_CRC
                    c.crc_seq = 0;
                    c.crc_checksum = 0;
                    c.crc_found = false;
#endif
                }
                c.inner.dispatch(c.inner.sink, ev);
            },
        };
    }
};

} // namespace detail

// ---------------------------------------------------------------------------
// IStreamingTransport
// ---------------------------------------------------------------------------

/// Type-erased build function for streaming request construction.
using BuildFn = void(*)(JsonBuilder&, void*);

/// Streaming transport interface for Notecard communication.
///
/// Derives from `ITransport` so streaming-protocol drivers participate in
/// the unified session interface. Adds two BuildFn-based virtuals
/// (`transact(BuildFn, …)` / `send(BuildFn, …)`) for the streaming
/// request-build path that emits JSON directly to the wire — useful when
/// you want to skip materialising the request as a string. The new
/// ITransport string-based methods (`transact(req, span)`,
/// `transact(req, sink)`, `send(req)`) get "not implemented" defaults
/// here so test stubs that only override the BuildFn methods continue to
/// compile; concrete protocol drivers (`Protocol`) override
/// them natively.
struct IStreamingTransport : public ITransport {
    /// Send a JSON request and SAX-parse the response into sink.
    virtual Result<void> transact(BuildFn build_fn, void* ctx,
                                  JsonSink& sink, uint32_t timeout_ms) = 0;

    /// Send a JSON command (fire-and-forget).
    virtual Result<void> send(BuildFn build_fn, void* ctx) = 0;

    /// Bring the inherited string-based ITransport overloads into scope so
    /// `transact(req, sink, …)` and `send(req)` aren't shadowed by the
    /// BuildFn overloads above.
    using ITransport::transact;
    using ITransport::send;

    /// Default ITransport bridges — IStreamingTransport stubs that only
    /// implement the BuildFn shape get "not implemented" responses on the
    /// string-based path. `Protocol` overrides these natively.
    Result<string_view> transact(string_view, span<char>, uint32_t) override {
        return make_error(Error::NotReady, NOTE_ERR("string transact not implemented"));
    }
    Result<void> transact(string_view, JsonSink&, uint32_t) override {
        return make_error(Error::NotReady, NOTE_ERR("string transact not implemented"));
    }
    Result<void> send(string_view) override {
        return make_error(Error::NotReady, NOTE_ERR("string send not implemented"));
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
// Protocol — protocol logic over a Hal
// ---------------------------------------------------------------------------

#if NOTE_STATIC_HAL
template<typename HalT>
class Protocol {
public:
    explicit Protocol(HalT& hal)
        : hal_(hal) {}

    Protocol(HalT& hal, uint32_t /*max_retries*/, uint32_t /*retry_delay_ms*/ = 500)
        : hal_(hal) {}
#elif NOTE_NO_POLYMORPHIC
class Protocol {
public:
    explicit Protocol(Hal& hal)
        : hal_(hal) {}

    Protocol(Hal& hal, uint32_t /*max_retries*/, uint32_t /*retry_delay_ms*/ = 500)
        : hal_(hal) {}
#else
class Protocol : public IStreamingTransport {
public:
    explicit Protocol(Hal& hal)
        : hal_(hal) {}

    Protocol(Hal& hal, uint32_t /*max_retries*/, uint32_t /*retry_delay_ms*/ = 500)
        : hal_(hal) {}
#endif

#if NOTE_DEBUG_ENABLED
#if NOTE_NO_POLYMORPHIC || NOTE_STATIC_HAL
    void set_debug(const DebugListener& d) { debug_ = d; }
#else
    void set_debug(const DebugListener& d) override { debug_ = d; }
#endif
#endif

#if NOTE_TXN_HANDSHAKE
    /// Register a transaction-handshake HAL to bracket every request with
    /// the SKU's RTX/CTX wake signal. See note/txn_handshake.hpp. Pass a
    /// TxnHandshake bound to the SKU's transaction pins; the transport
    /// brackets each transact/send/transact_raw call with start()/stop().
    void set_handshake(TxnHandshake& h) { handshake_ = &h; }
    /// Remove the transaction handshake (e.g. for testing).
    void clear_handshake() { handshake_ = nullptr; }
#endif

#if NOTE_STATIC_HAL
    HalT& hal() { return hal_; }
#elif NOTE_NO_POLYMORPHIC
    Hal& hal() { return hal_; }
#else
    Hal& hal() override { return hal_; }

    /// Virtual override for IStreamingTransport (used by Notecard).
    Result<void> transact(BuildFn build_fn, void* ctx,
                          JsonSink& sink, uint32_t timeout_ms) override {
        return transact_impl(build_fn, ctx, sink, timeout_ms);
    }

    // ── ITransport string-based overrides ──────────────────────────────
    //
    // The unified `ITransport` shape takes a pre-built request as
    // string_view. `Protocol` is the concrete protocol driver,
    // so it overrides all three natively rather than inheriting the
    // "not implemented" defaults from `IStreamingTransport`.

    using IStreamingTransport::transact;  // bring BuildFn overloads back into scope
    using IStreamingTransport::send;

    /// Buffered-style overload: ITransport `transact(req, span, timeout)`.
    Result<string_view> transact(string_view request, span<char> buf,
                                 uint32_t timeout_ms) override {
        return transact_raw(request, buf.data(), buf.size(), timeout_ms);
    }

    /// Streaming-style overload: ITransport `transact(req, sink, timeout)`.
    /// Transmits the pre-built request, then SAX-parses the response into
    /// `sink`. Same protocol path as the BuildFn variant on the receive
    /// side, just with the request bytes already materialised.
    Result<void> transact(string_view request, JsonSink& sink,
                          uint32_t timeout_ms) override {
        auto sv = send_raw(request);
        if (!sv) return Unexpected(sv.error());
        auto dispatch = make_sax_dispatch(sink);
        detail::NcErrorCapture nc_err;
        return receive_dispatch(dispatch, timeout_ms, nc_err);
    }

    /// Fire-and-forget: ITransport `send(req)`.
    Result<void> send(string_view request) override {
        return send_raw(request);
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
#if NOTE_TXN_HANDSHAKE
        detail::TxnHandshakeScope handshake_scope{handshake_, timeout_ms};
        if (!handshake_scope.ok())
            return make_error(Error::NotReady, Cause::Timeout, NOTE_ERR("txn handshake timeout"));
#endif
        if (!ensure_init()) {
            debug_transport(debug_, TransportEvent::ResetFailed, 0);
            return make_error(Error::NotReady, NOTE_ERR("not ready"));
        }

#if !NOTE_NO_CRC
        ++crc_seq_;
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
#if NOTE_TXN_HANDSHAKE
        detail::TxnHandshakeScope handshake_scope{handshake_, timeout_ms};
        if (!handshake_scope.ok())
            return make_error(Error::NotReady, Cause::Timeout, NOTE_ERR("txn handshake timeout"));
#endif
        if (!ensure_init()) {
            debug_transport(debug_, TransportEvent::ResetFailed, 0);
            return make_error(Error::NotReady, NOTE_ERR("not ready"));
        }

#if !NOTE_NO_CRC
        ++crc_seq_;
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
#if !NOTE_NO_POLYMORPHIC && !NOTE_STATIC_HAL
        override
#endif
    {
#if NOTE_TXN_HANDSHAKE
        detail::TxnHandshakeScope handshake_scope{handshake_, detail::kTxnHandshakeDefaultTimeoutMs};
        if (!handshake_scope.ok())
            return make_error(Error::NotReady, Cause::Timeout, NOTE_ERR("txn handshake timeout"));
#endif
        if (!ensure_init())
            return make_error(Error::NotReady, NOTE_ERR("not ready"));

#if !NOTE_NO_CRC
        ++crc_seq_;
#endif

        if (!stream_request(build_fn, ctx))
            return make_error(Error::SendFailed, Cause::HalError, NOTE_ERR("transmit failed"));
        return {};
    }

    /// Raw passthrough: transmit pre-formatted JSON + line terminator,
    /// read response line into caller's buffer. No SAX parsing — raw bytes.
    Result<string_view> transact_raw(string_view json, char* buf, size_t bufsize,
                                      uint32_t timeout_ms) {
#if NOTE_TXN_HANDSHAKE
        detail::TxnHandshakeScope handshake_scope{handshake_, timeout_ms};
        if (!handshake_scope.ok())
            return make_error(Error::NotReady, Cause::Timeout, NOTE_ERR("txn handshake timeout"));
#endif
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
#if NOTE_TXN_HANDSHAKE
        detail::TxnHandshakeScope handshake_scope{handshake_, detail::kTxnHandshakeDefaultTimeoutMs};
        if (!handshake_scope.ok())
            return make_error(Error::NotReady, Cause::Timeout, NOTE_ERR("txn handshake timeout"));
#endif
        if (!ensure_init())
            return make_error(Error::NotReady, NOTE_ERR("not ready"));

        if (!hal_.transmit(reinterpret_cast<const uint8_t*>(json.data()), json.size()))
            return make_error(Error::SendFailed, Cause::HalError, NOTE_ERR("transmit failed"));
        if (!hal_.write_line_terminator())
            return make_error(Error::SendFailed, Cause::HalError, NOTE_ERR("line terminator failed"));
        return {};
    }

    void reset()
#if !NOTE_NO_POLYMORPHIC && !NOTE_STATIC_HAL
        override
#endif
    {
        hal_.reset();
        initialized_ = false;
    }

    void abort()
#if !NOTE_NO_POLYMORPHIC && !NOTE_STATIC_HAL
        override
#endif
    {}

    Result<void> write(const uint8_t* data, size_t len)
#if !NOTE_NO_POLYMORPHIC && !NOTE_STATIC_HAL
        override
#endif
    {
        if (!hal_.transmit(data, len))
            return make_error(Error::SendFailed, Cause::HalError, NOTE_ERR("binary transmit failed"));
        return {};
    }

    Result<size_t> read(uint8_t* buf, size_t max_len, uint32_t timeout_ms)
#if !NOTE_NO_POLYMORPHIC && !NOTE_STATIC_HAL
        override
#endif
    {
        return read_hal(buf, max_len, timeout_ms);
    }

private:
    /// Read bytes with lookahead-first semantics. Any bytes stashed in
    /// lookahead_ by a prior frame_read (e.g. a second JSON frame or binary
    /// COBS data that arrived together with the JSON response in a single
    /// HAL chunk) are returned before the HAL is consulted again. This is
    /// the single choke point for consuming HAL bytes; read(), read_line(),
    /// drain_frame_boundary(), and frame_read() all go through here so they
    /// share the same pushback buffer and never drop queued bytes.
    Result<size_t> read_hal(uint8_t* buf, size_t max_len, uint32_t timeout_ms) {
#if !NOTE_NO_POLYMORPHIC
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

    /// Read bytes from HAL until newline, filling buf. Returns string_view into buf.
    /// If the response exceeds bufsize, drains the remainder and returns an error.
    Result<string_view> read_line(char* buf, size_t bufsize, uint32_t timeout_ms) {
        size_t pos = 0;
        bool overflow = false;
        for (;;) {
            uint8_t byte;
            auto rv = read_hal(&byte, 1, timeout_ms);
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
            auto rv = read_hal(&byte, 1, kDrainTimeoutMs);
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
#if NOTE_STATIC_HAL
            HalT& hal;
            bool ok = true;
            explicit Writer(HalT& h) : hal(h) {}
#else
            Hal& hal;
            bool ok = true;
            explicit Writer(Hal& h) : hal(h) {}
#endif
            bool write(const char* data, size_t len) override {
                if (!ok) return false;
                ok = hal.transmit(reinterpret_cast<const uint8_t*>(data), len);
                return ok;
            }
        } writer(hal_);

#if NOTE_JSONB
        // JSONB wire format: {: <COBS-encoded opcodes> :}\n
        // No CRC — COBS framing provides integrity.
        writer.write("{:", 2);
        CobsStreamWriter cobs(writer, jsonb::kCobsXor);
        StreamingJsonbBuilder builder(cobs);
        build_fn(builder, ctx);
        cobs.write(reinterpret_cast<const char*>(&jsonb::kEndObject), 1);
        cobs.flush();
        writer.write(":}", 2);
#else
#if !NOTE_NO_CRC
        {
            // Always send CRC — the Notecard echoes CRC back only when the
            // client includes it. Matches note-c's unconditional _crcAdd().
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
        }
#else
        {
            StreamingJsonBuilder builder(writer);
            build_fn(builder, ctx);
            writer.write("}", 1);
        }
#endif
#endif // NOTE_JSONB

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

#if NOTE_DEBUG_ENABLED
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
                auto r = read_hal(buf, max, t);
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
    #if NOTE_DEBUG_ENABLED
                if (debug_.on_wire)
                        debug_recv.append(reinterpret_cast<const char*>(buf), i);
#endif
#if !NOTE_NO_POLYMORPHIC
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
#if NOTE_DEBUG_ENABLED
            if (debug_.on_wire)
                debug_recv.append(reinterpret_cast<const char*>(buf), n);
#endif
            return n;
        };

#if NOTE_JSONB
        // JSONB wire format: read {: header, COBS-decode, parse JSONB opcodes.
        // No CRC — COBS framing provides integrity.
        {
            // Read and verify {: header (2 bytes).
            uint8_t header[2]{};
            size_t hdr_got = 0;
            while (hdr_got < 2) {
                auto r = frame_read(header + hdr_got, 2 - hdr_got, timeout_ms);
                if (!r) return Unexpected(r.error());
                if (*r == 0) return make_error(Error::ResponseLost, Cause::Timeout,
                                               NOTE_ERR("JSONB header timeout"));
                hdr_got += *r;
            }
            if (header[0] != '{' || header[1] != ':')
                return make_error(Error::ResponseLost, Cause::Unspecified,
                                  NOTE_ERR("expected JSONB header {:"));

            // COBS-decode and parse JSONB opcodes.
            // Use the frame_read's existing buffer for COBS decoding to
            // minimize stack usage (critical on AVR with 2KB stack).
            detail::CobsDecodingReader<decltype(frame_read)> cobs_reader(
                frame_read, timeout_ms);
            char jsonb_storage[128];
            SaxStreamBuf jsonb_buf(jsonb_storage);
            auto parse_err = jsonb_parse_streaming(
                cobs_reader, timeout_ms, jsonb_buf, wrapped);

            if (!frame_terminated && any_data_received)
                drain_frame_boundary();

            if (!parse_err.empty())
                return make_error(Error::ResponseLost, Cause::Unspecified, parse_err);
        }
#else
#if !NOTE_NO_CRC
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
#endif // NOTE_JSONB
#if NOTE_DEBUG_ENABLED
        // Emit wire receive debug event with the accumulated response bytes.
        if (debug_.on_wire && !debug_recv.empty())
            debug_wire(debug_, string_view(debug_recv.data(), debug_recv.size()),
                       WireDirection::Receive);
#endif
        return {};
    }

#if NOTE_STATIC_HAL
    HalT& hal_;
#else
    Hal& hal_;
#endif
    bool initialized_ = false;
#if NOTE_DEBUG_ENABLED
    DebugListener debug_{};
#else
    NoDebug debug_{};
#endif

#if !NOTE_NO_POLYMORPHIC
    // Lookahead buffer: bytes read from the HAL that arrived after \n
    // in the same chunk. Returned by subsequent read() calls (e.g. binary I/O).
    uint8_t lookahead_[64]{};
    size_t lookahead_pos_ = 0;
    size_t lookahead_len_ = 0;
#endif

#if !NOTE_NO_CRC
    bool crc_enabled_ = false;
    uint16_t crc_seq_ = 0;
#endif

#if NOTE_TXN_HANDSHAKE
    TxnHandshake* handshake_ = nullptr;
#endif
};

// Deprecated alias — `Protocol` was renamed from `StreamingTransport`
// in Phase 4 of the transport refactor. The old name is retained for
// one release; migrate at your convenience.
#if NOTE_STATIC_HAL
template<typename HalT>
using StreamingTransport = Protocol<HalT>;
#else
using StreamingTransport = Protocol;
#endif

} // namespace note
