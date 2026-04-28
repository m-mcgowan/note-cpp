#pragma once

/// @file transport.hpp
/// ITransport — unified Notecard session interface (Phase 3 of the
/// transport-rename arc; see docs/superpowers/plans/2026-04-27-transport-renames.md).
/// IBufferedTransport — deprecated bridge that adapts the old
/// `transact(req, timeout) -> Result<string_view>` shape to ITransport.
/// CallbackTransport — adapter for test lambdas (kept in note::test::).
///
/// `ITransport` exposes three transaction operations:
///   - `transact(req, span<char> buf, timeout)` — response copied into the
///     caller's buffer; returned `string_view` aliases that buffer.
///   - `transact(req, JsonSink& sink, timeout)` — response SAX-parsed into
///     a sink without an intermediate buffer.
///   - `send(req)` — fire-and-forget command.
///
/// Concrete protocol drivers (notably `Protocol`) override all
/// three natively for efficiency. Custom transports that only implement
/// the buffered overload get the sink overload for free via the default
/// impl, which reads into a local buffer then SAX-parses it.

#include <note/debug.hpp>
#include <note/error.hpp>
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
// ITransport — unified session interface
// ---------------------------------------------------------------------------

/// Unified Notecard session interface. Concrete transports must provide
/// the three transact/send overloads, plus reset/abort/hal. Binary I/O and
/// debug listener installation are optional (defaults: not supported / no-op).
///
/// The two `transact()` overloads correspond to two response presentations:
///   - `span<char>` — caller-owned buffer; response copied in, returned as a
///     `string_view` aliasing the leading bytes.
///   - `JsonSink&`  — SAX-parsed straight into the sink; no buffer needed.
struct ITransport {
    virtual ~ITransport() = default;

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
            return make_error(Error::ResponseLost, Cause::Unspecified, err);
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

    /// Install a debug listener for wire/timing/memory events. Default: no-op.
    virtual void set_debug(const DebugListener&) {}

    /// Write raw bytes (for binary COBS streaming). Default: not supported.
    virtual Result<void> write(const uint8_t*, size_t) {
        return make_error(Error::NotReady, "binary transfer not supported");
    }

    /// Read raw bytes (for binary COBS streaming). Default: not supported.
    virtual Result<size_t> read(uint8_t*, size_t, uint32_t) {
        return make_error(Error::NotReady, "binary transfer not supported");
    }

protected:
    /// Stack buffer size for the default `transact(req, sink)` bridge impl.
    /// Sized for typical Notecard responses; bumps if a derived class needs more.
    static constexpr size_t kDefaultBridgeBufferBytes = 1024;
};


// ---------------------------------------------------------------------------
// IBufferedTransport — DEPRECATED bridge
// ---------------------------------------------------------------------------

/// Bridges the old `transact(req, timeout) -> Result<string_view>` shape
/// (response in transport-owned storage) to the unified `ITransport`. New
/// transports should derive from `ITransport` directly; this class remains
/// for one release so existing buffered subclasses (notably
/// `note::test::CallbackTransport`) continue to compile without a
/// per-class rewrite.
struct IBufferedTransport : public ITransport {
    /// Old buffered transact: response is placed in transport-owned storage
    /// and returned by view. The view is valid until the next `transact()`
    /// call. Subclasses override THIS method; the new `ITransport` overloads
    /// below bridge to it automatically.
    virtual Result<string_view> transact(string_view request, uint32_t timeout_ms) = 0;

    /// Bridge: copies the transport-owned response into the caller's buffer.
    Result<string_view> transact(string_view request, span<char> buf,
                                 uint32_t timeout_ms) override {
        auto rv = transact(request, timeout_ms);
        if (!rv) return Unexpected(rv.error());
        if (rv->size() >= buf.size())
            return make_error(Error::Overflow, "response exceeds buffer");
        std::memcpy(buf.data(), rv->data(), rv->size());
        return string_view(buf.data(), rv->size());
    }

    /// Bridge: SAX-parses the transport-owned response into the sink.
    Result<void> transact(string_view request, JsonSink& sink,
                          uint32_t timeout_ms) override {
        auto rv = transact(request, timeout_ms);
        if (!rv) return Unexpected(rv.error());
        auto err = sax_parse(*rv, sink);
        if (!err.empty())
            return make_error(Error::ResponseLost, Cause::Unspecified, err);
        return {};
    }
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

/// Wraps function objects as a buffered transport for testing and examples.
///
/// @code
///     note::test::CallbackTransport transport(
///         [](string_view req, uint32_t) -> Result<string_view> { return "{}"; });
///     Notecard nc(backend, transport);
/// @endcode
class CallbackTransport : public IBufferedTransport {
public:
    using TransactFn = std::function<Result<string_view>(string_view, uint32_t)>;
    using SendFn = std::function<Result<void>(string_view)>;
    using WriteFn = std::function<Result<void>(const uint8_t*, size_t)>;
    using ReadFn = std::function<Result<size_t>(uint8_t*, size_t, uint32_t)>;

    explicit CallbackTransport(TransactFn transact_fn)
        : transact_(std::move(transact_fn)) {}

    CallbackTransport(TransactFn transact_fn, SendFn send_fn)
        : transact_(std::move(transact_fn)), send_(std::move(send_fn)) {}

    Result<string_view> transact(string_view request, uint32_t timeout_ms) override {
        return transact_(request, timeout_ms);
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

/// @deprecated Use `note::test::CallbackHal` instead.
using CallbackHal = test::CallbackHal;

/// @deprecated Use `note::test::CallbackTransport` instead.
using CallbackTransport = test::CallbackTransport;
#endif // NOTE_NO_STD_STRING

} // namespace note
