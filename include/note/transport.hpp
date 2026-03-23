#pragma once

/// @file transport.hpp
/// ITransport — abstract transport interface for Notecard communication.
/// AbstractTransport — base class with shared retry/CRC logic.
/// CallbackTransport — adapter for test lambdas.

#include <note/error.hpp>
#include <note/transport/detail/crc32.hpp>
#include <note/types.hpp>

#include <functional>
#include <string>

namespace note {

// ---------------------------------------------------------------------------
// ITransport — pure virtual transport contract
// ---------------------------------------------------------------------------

/// Transport interface for Notecard communication.
///
/// Implementations handle the wire protocol (serial, I2C) and buffer
/// management. The string_view returned from transact() points into the
/// transport's internal buffer and is valid until the next transact() call.
struct ITransport {
    virtual ~ITransport() = default;

    /// Send a JSON request and receive the response.
    virtual Result<string_view> transact(string_view request, uint32_t timeout_ms) = 0;

    /// Send a JSON command (fire-and-forget, no response expected).
    virtual Result<void> send(string_view request) = 0;

    /// Reset the transport to a known state (flush buffers, re-sync framing).
    /// Called before first use and between retry attempts.
    virtual void reset() = 0;

    /// Request abort of an in-progress transaction.
    /// Sets a flag that the transport's receive loop checks, causing it to
    /// return promptly with an error.
    virtual void abort() = 0;
};


// ---------------------------------------------------------------------------
// AbstractTransport — shared retry/CRC logic for Notecard wire protocols
// ---------------------------------------------------------------------------

/// Base class for Notecard transports (serial, I2C).
///
/// Provides the retry loop, CRC handling, and wire buffer management that
/// are common across all Notecard wire protocols. Subclasses implement
/// only the raw byte operations:
///
///   - do_transmit() — send bytes to the Notecard
///   - do_receive()  — receive a complete response line
///   - do_reset()    — hardware-level reset
///
/// The shared transact() implementation composes these with CRC and retry:
///   1. Prepare the wire buffer (CRC if enabled)
///   2. Retry loop: transmit → receive → CRC check
///   3. Return response or last error
///
/// send() is fire-and-forget: prepare + transmit, no receive.
class AbstractTransport : public ITransport {
public:
    // ── ITransport ────────────────────────────────────────────────────────

    Result<string_view> transact(string_view request, uint32_t timeout_ms) override {
        if (!initialized_) {
            if (!do_reset())
                return make_error(Error::NotReady, "Notecard not ready after reset");
            initialized_ = true;
        }

        prepare_wire(request);

        ErrorInfo last_error{Error::SendFailed, Cause::HalError, "transmit failed"};

        for (uint32_t attempt = 0; attempt <= max_retries(); ++attempt) {
            if (attempt > 0) delay(retry_delay_ms());

            if (!do_transmit(wire_.data(), wire_.size())) {
                last_error = {Error::SendFailed, Cause::HalError, "transmit failed"};
                do_reset();
                continue;
            }

            response_buf_.clear();
            auto result = do_receive(response_buf_, timeout_ms);
            if (!result) {
                last_error = result.error();
                continue;
            }

            if (transport::detail::crc_check_and_strip(response_buf_, crc_seq_, crc_enabled_)) {
                last_error = {Error::ResponseLost, Cause::CrcMismatch, "CRC mismatch"};
                continue;
            }

            return string_view(response_buf_);
        }

        return Unexpected(last_error);
    }

    Result<void> send(string_view request) override {
        if (!initialized_) {
            if (!do_reset())
                return make_error(Error::NotReady, "Notecard not ready after reset");
            initialized_ = true;
        }

        prepare_wire(request);

        if (!do_transmit(wire_.data(), wire_.size()))
            return make_error(Error::SendFailed, Cause::HalError, "transmit failed");

        return {};
    }

    void reset() override {
        do_reset();
        initialized_ = true;
    }

    void abort() override {}

protected:
    // ── Building blocks — subclasses implement these ──────────────────────

    /// Send raw bytes to the Notecard (segmented/chunked as needed).
    /// Returns false on hardware error.
    virtual bool do_transmit(const char* data, size_t len) = 0;

    /// Receive a complete response line into buf.
    /// buf is already cleared before this call.
    virtual Result<void> do_receive(std::string& buf, uint32_t timeout_ms) = 0;

    /// Reset the transport hardware to a known state.
    /// Returns true if the Notecard is ready for communication.
    virtual bool do_reset() = 0;

    /// Prepare the wire buffer from a JSON request string.
    /// Default: copies request and adds CRC if enabled.
    /// Override to append a protocol-specific line terminator (e.g. I2C \n).
    virtual void prepare_wire(string_view request) {
        wire_.assign(request.data(), request.size());
        if (crc_enabled_) {
            ++crc_seq_;
            wire_ = transport::detail::crc_add(std::move(wire_), crc_seq_);
        }
    }

    // ── Policy access — subclasses provide ────────────────────────────────

    virtual uint32_t max_retries() const = 0;
    virtual uint32_t retry_delay_ms() const = 0;
    virtual void delay(uint32_t ms) = 0;

    // ── Shared state ──────────────────────────────────────────────────────

    bool initialized_ = false;
    bool crc_enabled_ = false;
    uint16_t crc_seq_ = 0;
    std::string wire_;          // reused across requests
    std::string response_buf_;  // reused — string_view return points here
};


// ---------------------------------------------------------------------------
// CallbackTransport — adapter for test lambdas
// ---------------------------------------------------------------------------

/// Wraps function objects as an ITransport for testing and examples.
///
/// @code
///     CallbackTransport transport(
///         [](string_view req, uint32_t) -> Result<string_view> { return "{}"; });
///     Notecard nc(backend, transport);
/// @endcode
class CallbackTransport : public ITransport {
public:
    using TransactFn = std::function<Result<string_view>(string_view, uint32_t)>;
    using SendFn = std::function<Result<void>(string_view)>;

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

    void reset() override {}
    void abort() override {}

private:
    TransactFn transact_;
    SendFn send_;
};

} // namespace note
