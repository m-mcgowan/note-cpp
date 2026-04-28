#pragma once

/// @file transport.hpp
/// IBufferedTransport — buffered transport interface.
/// CallbackTransport — adapter for test lambdas.

#include <note/error.hpp>
#include <note/transport_hal.hpp>
#include <note/types.hpp>

#if !NOTE_NO_STD_STRING
#include <functional>
#endif

namespace note {

// ---------------------------------------------------------------------------
// IBufferedTransport — buffered transport contract
// ---------------------------------------------------------------------------

/// Buffered transport interface for Notecard communication.
///
/// Implementations handle the wire protocol and buffer management.
/// The string_view returned from transact() points into the transport's
/// internal buffer and is valid until the next transact() call.
struct IBufferedTransport {
    virtual ~IBufferedTransport() = default;

    /// Send a JSON request and receive the response.
    virtual Result<string_view> transact(string_view request, uint32_t timeout_ms) = 0;

    /// Send a JSON command (fire-and-forget, no response expected).
    virtual Result<void> send(string_view request) = 0;

    /// Reset the transport to a known state.
    virtual void reset() = 0;

    /// Request abort of an in-progress transaction.
    virtual void abort() = 0;

    /// Access the underlying byte HAL — for timing primitives, bus reset,
    /// and any low-level operations that don't go through the protocol.
    virtual Hal& hal() = 0;

    /// Write raw bytes (for binary COBS streaming). Default: not supported.
    virtual Result<void> write(const uint8_t*, size_t) {
        return make_error(Error::NotReady, "binary transfer not supported");
    }

    /// Read raw bytes (for binary COBS streaming). Default: not supported.
    virtual Result<size_t> read(uint8_t*, size_t, uint32_t) {
        return make_error(Error::NotReady, "binary transfer not supported");
    }
};

/// @deprecated Use IBufferedTransport directly.
using ITransport = IBufferedTransport;


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

/// Wraps function objects as an IBufferedTransport for testing and examples.
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
