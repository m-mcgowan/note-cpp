#pragma once

/// @file transport.hpp
/// IBufferedTransport — buffered transport interface.
/// CallbackTransport — adapter for test lambdas.

#include <note/error.hpp>
#include <note/types.hpp>

#if !defined(NOTE_NO_STD_STRING) && !defined(NOTE_NO_STD_FUNCTION)
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


#if !defined(NOTE_NO_STD_STRING) && !defined(NOTE_NO_STD_FUNCTION)
// ---------------------------------------------------------------------------
// CallbackTransport — adapter for test lambdas
// ---------------------------------------------------------------------------

/// Wraps function objects as an IBufferedTransport for testing and examples.
///
/// @code
///     CallbackTransport transport(
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

private:
    TransactFn transact_;
    SendFn send_;
    WriteFn write_;
    ReadFn read_;
};
#endif // !NOTE_NO_STD_STRING && !NOTE_NO_STD_FUNCTION

} // namespace note
