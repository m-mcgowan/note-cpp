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

    /// Write raw bytes to the transport. No framing, CRC, or line terminators.
    /// Used for binary (COBS) data streaming.
    virtual Result<void> write(const uint8_t*, size_t) {
        return make_error(Error::NotReady, "binary transfer not supported");
    }

    /// Read available bytes from the transport (up to max_len).
    /// Returns number of bytes read. Does not wait for a complete message —
    /// returns whatever is available, or blocks up to timeout_ms for at least
    /// one byte. Used for streaming binary (COBS) receive.
    virtual Result<size_t> read(uint8_t*, size_t, uint32_t) {
        return make_error(Error::NotReady, "binary transfer not supported");
    }
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
    /// Set an external receive buffer. When set, transact() reads into
    /// this buffer instead of the internal response_buf_ (zero heap).
    /// Pass nullptr to revert to the internal buffer.
    void set_receive_buffer(char* buf, size_t len) {
        ext_buf_ = buf;
        ext_buf_size_ = len;
    }

    // TODO: set_wire_buffer() for zero-heap outbound path.
    // Blocked on crc_add() which is std::string-based.
    // The streaming transport (transact_streaming) bypasses wire_ entirely.

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

            if (ext_buf_) {
                // Phase 2: read into caller buffer via do_read()
                auto rv = receive_into(ext_buf_, ext_buf_size_, timeout_ms);
                if (!rv) { last_error = rv.error(); continue; }
                return *rv;
            } else {
                // Original path: read into response_buf_ via do_receive()
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

    /// Transact into a caller-provided buffer (Phase 2).
    /// Returns a string_view into buf (not response_buf_).
    /// The caller owns the buffer — no transport-level allocation.
    Result<string_view> transact_into(string_view request, uint32_t timeout_ms,
                                       char* buf, size_t buf_size) {
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

            // Read into caller buffer via do_read(), stop at \n.
            size_t pos = 0;
            bool found_eol = false;
            bool read_error = false;

            while (!found_eol && pos < buf_size) {
                auto r = do_read(reinterpret_cast<uint8_t*>(buf + pos),
                                 buf_size - pos, timeout_ms);
                if (!r) {
                    last_error = r.error();
                    read_error = true;
                    break;
                }
                size_t n = *r;
                for (size_t i = 0; i < n; ++i) {
                    if (buf[pos + i] == '\n') {
                        pos += i;  // don't include \n
                        found_eol = true;
                        break;
                    }
                }
                if (!found_eol) pos += n;
            }

            if (read_error) continue;

            // Strip trailing \r
            while (pos > 0 && buf[pos - 1] == '\r') --pos;

            // CRC check (operates on buf, not response_buf_)
            std::string crc_buf(buf, pos);
            if (transport::detail::crc_check_and_strip(crc_buf, crc_seq_, crc_enabled_)) {
                last_error = {Error::ResponseLost, Cause::CrcMismatch, "CRC mismatch"};
                continue;
            }

            // If CRC was stripped, copy the stripped version back
            if (crc_enabled_) {
                memcpy(buf, crc_buf.data(), crc_buf.size());
                pos = crc_buf.size();
            }

            return string_view(buf, pos);
        }

        return Unexpected(last_error);
    }

    /// Streaming transact: send request, read response chunk-by-chunk.
    /// Each chunk is passed to on_chunk(data, len). Reading stops at '\n'.
    /// CRC is checked if enabled (uses response_buf_ internally for this).
    template<typename ChunkFn>
    Result<void> transact_streaming(string_view request, uint32_t timeout_ms,
                                     uint8_t* chunk_buf, size_t chunk_size,
                                     ChunkFn&& on_chunk) {
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

            // Read response chunk-by-chunk until \n delimiter.
            response_buf_.clear();
            bool found_eol = false;
            bool read_error = false;

            while (!found_eol) {
                auto r = do_read(chunk_buf, chunk_size, timeout_ms);
                if (!r) {
                    last_error = r.error();
                    read_error = true;
                    break;
                }
                size_t n = *r;
                for (size_t i = 0; i < n; ++i) {
                    if (chunk_buf[i] == '\n') {
                        if (i > 0) {
                            on_chunk(chunk_buf, i);
                            response_buf_.append(reinterpret_cast<const char*>(chunk_buf), i);
                        }
                        found_eol = true;
                        break;
                    }
                }
                if (!found_eol) {
                    on_chunk(chunk_buf, n);
                    response_buf_.append(reinterpret_cast<const char*>(chunk_buf), n);
                }
            }

            if (read_error) continue;

            // Strip trailing \r and check CRC
            while (!response_buf_.empty() && response_buf_.back() == '\r')
                response_buf_.pop_back();
            if (transport::detail::crc_check_and_strip(response_buf_, crc_seq_, crc_enabled_)) {
                last_error = {Error::ResponseLost, Cause::CrcMismatch, "CRC mismatch"};
                continue;
            }

            return {};
        }

        return Unexpected(last_error);
    }

    void reset() override {
        do_reset();
        initialized_ = true;
    }

    void abort() override {}

    Result<void> write(const uint8_t* data, size_t len) override {
        if (!do_write(data, len))
            return make_error(Error::SendFailed, Cause::HalError, "binary transmit failed");
        return {};
    }

    Result<size_t> read(uint8_t* buf, size_t max_len, uint32_t timeout_ms) override {
        return do_read(buf, max_len, timeout_ms);
    }

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

    /// Write raw bytes without protocol framing.
    /// Default delegates to do_transmit() — override if protocol transmit
    /// adds framing (e.g. serial appends \r\n).
    virtual bool do_write(const uint8_t* data, size_t len) {
        return do_transmit(reinterpret_cast<const char*>(data), len);
    }

    /// Read available bytes from the transport (up to max_len).
    /// Returns bytes read. Blocks up to timeout_ms for at least one byte.
    virtual Result<size_t> do_read(uint8_t* buf, size_t max_len, uint32_t timeout_ms) = 0;

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

    // Wire and response buffers. When external buffers are set (via
    // set_wire_buffer / set_receive_buffer), the std::string members are
    // unused. This allows fully heap-free operation when the caller
    // controls buffer placement.
    std::string wire_;          // fallback outbound buffer
    std::string response_buf_;  // fallback inbound buffer

    // External buffers (Phase 2 — caller-provided, zero heap)
    char* ext_buf_ = nullptr;
    size_t ext_buf_size_ = 0;

private:
    /// Read into a caller-provided buffer via do_read(), stopping at \n.
    /// Strips trailing \r and checks CRC. Returns string_view into buf.
    Result<string_view> receive_into(char* buf, size_t buf_size, uint32_t timeout_ms) {
        size_t pos = 0;
        bool found_eol = false;

        while (!found_eol && pos < buf_size) {
            auto r = do_read(reinterpret_cast<uint8_t*>(buf + pos),
                             buf_size - pos, timeout_ms);
            if (!r) return Unexpected(r.error());

            size_t n = *r;
            for (size_t i = 0; i < n; ++i) {
                if (buf[pos + i] == '\n') {
                    pos += i;  // don't include \n
                    found_eol = true;
                    break;
                }
            }
            if (!found_eol) pos += n;
        }

        // Strip trailing \r
        while (pos > 0 && buf[pos - 1] == '\r') --pos;

        // CRC check (temporary std::string — acceptable for v0.1)
        std::string crc_buf(buf, pos);
        if (transport::detail::crc_check_and_strip(crc_buf, crc_seq_, crc_enabled_)) {
            return make_error(Error::ResponseLost, Cause::CrcMismatch, "CRC mismatch");
        }

        if (crc_enabled_) {
            memcpy(buf, crc_buf.data(), crc_buf.size());
            pos = crc_buf.size();
        }

        return string_view(buf, pos);
    }
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

} // namespace note
