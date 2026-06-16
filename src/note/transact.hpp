#pragma once

/// @file transact.hpp
/// ITransact — unified Notecard session interface (Phase 3 of the
/// transport-rename arc; see docs/superpowers/plans/2026-04-27-transport-renames.md).
/// note::test::CallbackTransport — adapter for test lambdas.
///
/// `ITransact` exposes six transaction operations across two request
/// shapes:
///
///   string_view shape (pre-built JSON):
///     - `transact(req, span<char> buf, timeout)` — response copied into the
///       caller's buffer; returned `string_view` aliases that buffer.
///     - `transact(req, JsonSink& sink, timeout)` — response SAX-parsed into
///       a sink without an intermediate buffer.
///     - `send(req)` — fire-and-forget command.
///
///   RequestSource shape (streaming-built JSON):
///     - `transact(RequestSource, span<char>, timeout)`
///     - `transact(RequestSource, JsonSink&, timeout)`
///     - `send(RequestSource)`
///
/// Concrete protocol drivers (notably `Protocol`) override all six natively
/// for efficiency. Custom transports typically override only the
/// `transact(req, span, t)` and `send(req)` virtuals; the other overloads
/// (sink, RequestSource) get default impls that materialise into a stack
/// scratch buffer and forward to the buffered overload.

#include <note/debug.hpp>
#include <note/error.hpp>
#include <note/json.hpp>
#include <note/json_sax.hpp>
#include <note/span.hpp>
#include <note/transport_hal.hpp>
#include <note/types.hpp>

#include <cstring>
#if !NOTE_NO_STD_STRING
#include <functional>
#endif

namespace note {

// ---------------------------------------------------------------------------
// RequestSource — type-erased emitter for JSON request bytes
// ---------------------------------------------------------------------------

/// Type-erased request source: a function pointer + context that paints
/// request bytes through a `JsonWriter`. Two-word POD; no vtable, no
/// allocation.
///
/// `note/request_source.hpp` ships the builder-shape adapter
/// (`BuilderRequestSource<F>`) — runs a user callable against a
/// `StreamingJsonBuilder` layered over the writer. A verbatim string-shape
/// adapter is deferred until Phase 5b's field router lands; until then,
/// pre-built JSON keeps going through the legacy `transact(string_view, …)`
/// overloads on `ITransact`.
///
/// `RequestSource` is the unified shape that step 8 of Phase 5a collapses
/// `ITransact` to. Today (step 3) it lives alongside the legacy
/// `string_view` / `BuildFn` shapes; concrete drivers (`Protocol`) override
/// it natively, and most consumers continue to use the legacy shapes until
/// later steps in the arc migrate them.
struct RequestSource {
    using EmitFn = void(*)(JsonWriter& w, void* ctx);

    EmitFn emit_fn;
    void*  ctx;

    void emit(JsonWriter& w) const { emit_fn(w, ctx); }
};

// ---------------------------------------------------------------------------
// ITransact — unified session interface
// ---------------------------------------------------------------------------

/// Unified Notecard session interface. Concrete transports must provide
/// the three transact/send overloads, plus reset/abort/hal. Binary I/O and
/// debug listener installation are optional (defaults: not supported / no-op).
///
/// The two `transact()` overloads correspond to two response presentations:
///   - `span<char>` — caller-owned buffer; response copied in, returned as a
///     `string_view` aliasing the leading bytes.
///   - `JsonSink&`  — SAX-parsed straight into the sink; no buffer needed.
struct ITransact {
    virtual ~ITransact() = default;

    /// Send a JSON request and copy the response into the caller's buffer.
    virtual Result<string_view> transact(string_view request, span<char> buf,
                                         uint32_t timeout_ms) = 0;

    /// Send a JSON request and SAX-parse the response into `sink`.
    ///
    /// Default impl: allocate a stack buffer, delegate to the buffered
    /// overload, then `sax_parse` the buffer into `sink`. Override for a
    /// streaming-native implementation that avoids the intermediate buffer.
    virtual Result<void> transact(string_view request, JsonSink& sink,
                                  uint32_t timeout_ms) {
        char buf[kDefaultBridgeBufferBytes];
        auto rv = transact(request, span<char>(buf, sizeof(buf)), timeout_ms);
        if (!rv) return Unexpected(rv.error());
        auto err = sax_parse(*rv, sink);
        if (!err.empty())
            // SAX parse over a fully-buffered response — a parse failure
            // here is a JSON-level error, distinct from the mid-stream
            // ResponseLost the Protocol path reports for transport
            // corruption (partial reads, CRC mismatch, etc.).
            return make_error(Error::Json, Cause::Unspecified, err);
        return {};
    }

    /// Send a JSON command (fire-and-forget, no response expected).
    virtual Result<void> send(string_view request) = 0;

    /// Reset the transport to a known state (e.g. re-init handshake).
    virtual void reset() = 0;

    /// Request abort of an in-progress transaction.
    virtual void abort() = 0;

    /// Access the underlying byte HAL — for timing primitives, bus reset,
    /// and any low-level operations that don't go through the protocol.
    virtual Hal& hal() = 0;

    /// Called once at the start of an outermost operation to assert the
    /// RTX/CTX readiness gate (if any). Returns true when the transport is
    /// ready to proceed; returns false on timeout (callers should consider
    /// the operation not ready, but correctness is maintained — subsequent
    /// transact() calls will simply time out at the wire level).
    ///
    /// Raw `ITransact` users who drive the transport directly (outside
    /// `Notecard::run_operation`) must bracket their operations explicitly:
    ///   transport.begin_operation(timeout);
    ///   transport.transact(...);   // one or more exchanges
    ///   transport.end_operation();
    ///
    /// Default: always returns true (no-op, no readiness wait needed).
    virtual bool begin_operation(uint32_t /*timeout_ms*/) { return true; }

    /// Called once at the end of an outermost operation to release the RTX
    /// signal. Default: no-op.
    virtual void end_operation() {}

    /// Install a debug listener for wire/timing/memory events. Default: no-op.
    virtual void set_debug(const DebugListener&) {}

    /// Acquire the bus lock for a multi-call raw byte sequence (e.g. a binary
    /// COBS payload stream) so the lock is held across the entire stream rather
    /// than released between individual write()/read() calls. Paired with
    /// end_bus_hold(). Must be called while no per-exchange lock is already
    /// held (i.e. outside a transact/send call). Default: no-op.
    virtual void begin_bus_hold() {}

    /// Release the bus lock acquired by begin_bus_hold(). Default: no-op.
    virtual void end_bus_hold() {}

    /// Write raw bytes (for binary COBS streaming). Default: not supported.
    virtual Result<void> write(const uint8_t*, size_t) {
        return make_error(Error::NotReady, "binary transfer not supported");
    }

    /// Read raw bytes (for binary COBS streaming). Default: not supported.
    virtual Result<size_t> read(uint8_t*, size_t, uint32_t) {
        return make_error(Error::NotReady, "binary transfer not supported");
    }

    // ─── RequestSource overloads — materialise-and-forward defaults ───
    //
    // The default impls materialise the source into a stack scratch buffer,
    // append the closing `}` (the source emits fields only; the protocol
    // layer normally adds the brace), and forward to the corresponding
    // string_view virtual. `Protocol` overrides these natively for
    // zero-buffer streaming; transports that only support pre-built strings
    // (notably the test CallbackTransport, mock transports, the note-c
    // bridge) inherit the bridges automatically.

    /// RequestSource bridge: materialise → forward to `transact(string_view, span, t)`.
    virtual Result<string_view> transact(RequestSource src, span<char> buf,
                                         uint32_t timeout_ms) {
        char scratch[kDefaultBridgeBufferBytes];
        JsonBufferWriter writer(scratch, sizeof(scratch));
        src.emit(writer);
        writer.write('}');
        if (writer.overflow())
            return make_error(Error::Overflow, NOTE_ERR("request exceeds bridge scratch buffer"));
        return transact(writer.view(), buf, timeout_ms);
    }

    /// RequestSource bridge: materialise → forward to `transact(string_view, sink, t)`.
    virtual Result<void> transact(RequestSource src, JsonSink& sink,
                                  uint32_t timeout_ms) {
        char scratch[kDefaultBridgeBufferBytes];
        JsonBufferWriter writer(scratch, sizeof(scratch));
        src.emit(writer);
        writer.write('}');
        if (writer.overflow())
            return make_error(Error::Overflow, NOTE_ERR("request exceeds bridge scratch buffer"));
        return transact(writer.view(), sink, timeout_ms);
    }

    /// RequestSource bridge: materialise → forward to `send(string_view)`.
    virtual Result<void> send(RequestSource src) {
        char scratch[kDefaultBridgeBufferBytes];
        JsonBufferWriter writer(scratch, sizeof(scratch));
        src.emit(writer);
        writer.write('}');
        if (writer.overflow())
            return make_error(Error::Overflow, NOTE_ERR("request exceeds bridge scratch buffer"));
        return send(writer.view());
    }

protected:
    /// Stack buffer size for the default `transact(req, sink)` bridge impl.
    /// Sized for typical Notecard responses; bumps if a derived class needs more.
    static constexpr size_t kDefaultBridgeBufferBytes = 1024;
};


#if !NOTE_NO_STD_STRING
namespace test {

// ---------------------------------------------------------------------------
// CallbackHal — no-op test Hal paired with CallbackTransport
// ---------------------------------------------------------------------------

/// Minimum-viable test `Hal`. Not wired to real hardware — every method is
/// a no-op return-default. Used internally by `CallbackTransport` so that
/// `Notecard::hal()` returns a valid reference when a callback transport
/// is installed; tests that don't exercise the HAL never need to touch it.
class CallbackHal : public Hal {
public:
    bool transmit(const uint8_t*, size_t) override { return true; }
    Result<size_t> read(uint8_t*, size_t, uint32_t) override { return Result<size_t>{size_t{0}}; }
    bool reset() override { return true; }
    bool write_line_terminator() override { return true; }
    uint32_t millis() override { return 0; }
    void delay(uint32_t) override {}
};

// ---------------------------------------------------------------------------
// CallbackTransport — adapter for test lambdas
// ---------------------------------------------------------------------------

/// Wraps function objects as a transport for testing and examples.
///
/// The TransactFn returns a `string_view` aliasing transport-owned storage
/// (e.g. a static string the lambda captures); the `transact(req, span, t)`
/// override copies into the caller's buffer. The `transact(req, sink, t)`
/// and RequestSource overloads inherit ITransact's defaults — buffered
/// transact + SAX-parse / materialise + forward.
///
/// @code
///     note::test::CallbackTransport transport(
///         [](string_view req, uint32_t) -> Result<string_view> { return "{}"; });
///     Notecard nc(backend, transport);
/// @endcode
class CallbackTransport : public ITransact {
public:
    using TransactFn = std::function<Result<string_view>(string_view, uint32_t)>;
    using SendFn = std::function<Result<void>(string_view)>;
    using WriteFn = std::function<Result<void>(const uint8_t*, size_t)>;
    using ReadFn = std::function<Result<size_t>(uint8_t*, size_t, uint32_t)>;

    explicit CallbackTransport(TransactFn transact_fn)
        : transact_(std::move(transact_fn)) {}

    CallbackTransport(TransactFn transact_fn, SendFn send_fn)
        : transact_(std::move(transact_fn)), send_(std::move(send_fn)) {}

    using ITransact::transact;
    using ITransact::send;

    Result<string_view> transact(string_view request, span<char> buf,
                                 uint32_t timeout_ms) override {
        auto rv = transact_(request, timeout_ms);
        if (!rv) return Unexpected(rv.error());
        if (rv->size() >= buf.size())
            return make_error(Error::Overflow, NOTE_ERR(
                "response exceeds buffer; enlarge with nc.set_response_buffer() "
                "or wire .into(JsonSink&) for streaming"));
        std::memcpy(buf.data(), rv->data(), rv->size());
        return string_view(buf.data(), rv->size());
    }

    Result<void> send(string_view request) override {
        if (send_) return send_(request);
        auto r = transact_(request, 0);
        if (!r) return Unexpected(r.error());
        return {};
    }

    Result<void> write(const uint8_t* data, size_t len) override {
        if (write_) return write_(data, len);
        return make_error(Error::NotReady, "binary transfer not supported");
    }

    Result<size_t> read(uint8_t* buf, size_t max_len, uint32_t timeout_ms) override {
        if (read_) return read_(buf, max_len, timeout_ms);
        return make_error(Error::NotReady, "binary transfer not supported");
    }

    void set_write(WriteFn fn) { write_ = std::move(fn); }
    void set_read(ReadFn fn) { read_ = std::move(fn); }

    void reset() override {}
    void abort() override {}

    Hal& hal() override { return hal_; }

private:
    TransactFn transact_;
    SendFn send_;
    WriteFn write_;
    ReadFn read_;
    CallbackHal hal_;
};

} // namespace test

#endif // NOTE_NO_STD_STRING

} // namespace note
