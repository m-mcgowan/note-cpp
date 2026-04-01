#pragma once

/// @file transport.hpp
/// ITransport — abstract transport interface for Notecard communication.
/// AbstractTransport — base class with shared retry/CRC logic.
/// CallbackTransport — adapter for test lambdas.

#include <note/error.hpp>
#include <note/json.hpp>
#include <note/json_sax_streaming.hpp>
#include <note/transport/detail/crc32.hpp>
#include <note/types.hpp>

#ifndef NOTE_NO_STD_STRING
#include <functional>
#include <string>
#endif

namespace note {

// ---------------------------------------------------------------------------
// IBufferedTransport — buffered transport contract
// ---------------------------------------------------------------------------

/// Buffered transport interface for Notecard communication.
///
/// Implementations handle the wire protocol (serial, I2C) and buffer
/// management. The string_view returned from transact() points into the
/// transport's internal buffer and is valid until the next transact() call.
struct IBufferedTransport {
    virtual ~IBufferedTransport() = default;

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

/// @deprecated Use IBufferedTransport directly.
using ITransport = IBufferedTransport;


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
class AbstractTransport : public IBufferedTransport {
public:
    /// Set an external receive buffer. When set, transact() reads into
    /// this buffer instead of the internal response_buf_ (zero heap).
    /// Pass nullptr to revert to the internal buffer.
    void set_receive_buffer(char* buf, size_t len) {
        ext_buf_ = buf;
        ext_buf_size_ = len;
    }

    // ── Streaming build types ────────────────────────────────────────────

    /// JsonWriter adapter that writes raw bytes through do_write().
    struct RawWriter : JsonWriter {
        using JsonWriter::write;
        AbstractTransport& transport;
        bool ok = true;

        explicit RawWriter(AbstractTransport& t) : transport(t) {}

        bool write(const char* data, size_t len) override {
            if (!ok) return false;
            ok = transport.do_write(
                reinterpret_cast<const uint8_t*>(data), len);
            return ok;
        }
    };

    /// JsonWriter that accumulates CRC32 and forwards to an inner writer.
    struct CrcWriter : JsonWriter {
        using JsonWriter::write;
        JsonWriter& inner;
        uint32_t state;

        explicit CrcWriter(JsonWriter& w)
            : inner(w), state(transport::detail::crc32_begin()) {}

        bool write(const char* data, size_t len) override {
            state = transport::detail::crc32_update(state, data, len);
            return inner.write(data, len);
        }

        /// Feed '}' to CRC without writing it, return finalized checksum.
        /// The CRC covers the original JSON {…} including closing brace.
        uint32_t finalize_with_brace() {
            state = transport::detail::crc32_update(state, "}", 1);
            return transport::detail::crc32_finalize(state);
        }
    };

    /// SAX sink filter that intercepts the "crc" field from a response.
    /// Extracts sequence number and checksum; does not forward to inner sink.
    struct CrcFieldSink : FilterJsonSink {
        uint16_t seq_ = 0;
        uint32_t checksum_ = 0;
        bool found_ = false;

        explicit CrcFieldSink(JsonSink& inner) : FilterJsonSink(inner) {}

        bool has_crc() const { return found_; }
        uint16_t seq() const { return seq_; }
        uint32_t checksum() const { return checksum_; }

        void on_string(string_view key, string_view value) override {
            if (key == "crc") {
                if (value.size() == 13 && value[4] == ':') {
                    seq_ = static_cast<uint16_t>(
                        transport::detail::read_hex(value.data(), 4));
                    checksum_ = static_cast<uint32_t>(
                        transport::detail::read_hex(value.data() + 5, 8));
                    found_ = true;
                }
                return;
            }
            inner_.on_string(key, value);
        }

        void reset() override {
            seq_ = 0;
            checksum_ = 0;
            found_ = false;
            FilterJsonSink::reset();
        }
    };

    // ── Streaming build — write JSON directly to transport ────────────────
    //
    // transact_build / send_build bypass the wire buffer entirely. The
    // StreamingJsonBuilder writes each byte directly through do_write(),
    // with optional CRC accumulation. On retry, the build function is
    // re-invoked to regenerate the request.

    /// Build a JSON request via streaming builder and transact.
    /// build_fn receives a JsonBuilder& and populates request fields.
    /// The transport handles '{', '}', CRC, line terminator, and framing.
    template<typename BuildFn>
    Result<string_view> transact_build(BuildFn build_fn, uint32_t timeout_ms) {
        if (!initialized_) {
            if (!do_reset())
                return make_error(Error::NotReady, "Notecard not ready after reset");
            initialized_ = true;
        }

        if (crc_enabled_) ++crc_seq_;

        ErrorInfo last_error{Error::SendFailed, Cause::HalError, "transmit failed"};

        for (uint32_t attempt = 0; attempt <= max_retries(); ++attempt) {
            if (attempt > 0) {
                delay(retry_delay_ms());
                do_reset();
            }

            if (!stream_request(build_fn)) {
                last_error = {Error::SendFailed, Cause::HalError, "transmit failed"};
                continue;
            }

            auto rv = receive_response(timeout_ms);
            if (!rv) {
                last_error = rv.error();
                continue;
            }
            return *rv;
        }

        return Unexpected(last_error);
    }

    /// Build a JSON command via streaming builder (fire-and-forget).
    template<typename BuildFn>
    Result<void> send_build(BuildFn build_fn) {
        if (!initialized_) {
            if (!do_reset())
                return make_error(Error::NotReady, "Notecard not ready after reset");
            initialized_ = true;
        }

        if (crc_enabled_) ++crc_seq_;

        if (!stream_request(build_fn))
            return make_error(Error::SendFailed, Cause::HalError, "transmit failed");

        return {};
    }

    /// Stream send + stream receive + retry.
    /// build_fn populates a JsonBuilder& with request fields.
    /// sink receives SAX events from the response (except "crc").
    /// Calls sink.reset() before each retry attempt.
    template<typename BuildFn>
    Result<void> transact_streaming(BuildFn build_fn, JsonSink& sink,
                                     uint32_t timeout_ms) {
        if (!initialized_) {
            if (!do_reset())
                return make_error(Error::NotReady, "Notecard not ready after reset");
            initialized_ = true;
        }

        if (crc_enabled_) ++crc_seq_;

        ErrorInfo last_error{Error::SendFailed, Cause::HalError, "transmit failed"};

        for (uint32_t attempt = 0; attempt <= max_retries(); ++attempt) {
            if (attempt > 0) {
                delay(retry_delay_ms());
                do_reset();
                sink.reset();
            }

            if (!stream_request(build_fn)) {
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

            if (!do_transmit(wire_data(), wire_len_)) {
                last_error = {Error::SendFailed, Cause::HalError, "transmit failed"};
                do_reset();
                continue;
            }

            if (ext_buf_) {
                // Phase 2: read into caller buffer via do_read()
                auto rv = receive_into(ext_buf_, ext_buf_size_, timeout_ms);
                if (!rv) { last_error = rv.error(); continue; }
                return *rv;
            }
#ifndef NOTE_NO_STD_STRING
            else {
                // Original path: read into response_buf_ via do_receive()
                response_buf_.clear();
                auto result = do_receive(response_buf_, timeout_ms);
                if (!result) {
                    last_error = result.error();
                    continue;
                }

                size_t rsp_len = response_buf_.size();
                if (transport::detail::crc_check_and_strip(
                        response_buf_.data(), rsp_len, crc_seq_, crc_enabled_)) {
                    last_error = {Error::ResponseLost, Cause::CrcMismatch, "CRC mismatch"};
                    continue;
                }
                response_buf_.resize(rsp_len);

                return string_view(response_buf_);
            }
#endif
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

        if (!do_transmit(wire_data(), wire_len_))
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

            if (!do_transmit(wire_data(), wire_len_)) {
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

            // CRC check (in-place on caller buffer)
            if (transport::detail::crc_check_and_strip(buf, pos, crc_seq_, crc_enabled_)) {
                last_error = {Error::ResponseLost, Cause::CrcMismatch, "CRC mismatch"};
                continue;
            }

            return string_view(buf, pos);
        }

        return Unexpected(last_error);
    }

#ifndef NOTE_NO_STD_STRING
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

            if (!do_transmit(wire_data(), wire_len_)) {
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
            size_t rsp_len2 = response_buf_.size();
            if (transport::detail::crc_check_and_strip(
                    response_buf_.data(), rsp_len2, crc_seq_, crc_enabled_)) {
                last_error = {Error::ResponseLost, Cause::CrcMismatch, "CRC mismatch"};
                continue;
            }
            response_buf_.resize(rsp_len2);

            return {};
        }

        return Unexpected(last_error);
    }
#endif // NOTE_NO_STD_STRING

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

#ifndef NOTE_NO_STD_STRING
    /// Receive a complete response line into buf.
    /// buf is already cleared before this call.
    virtual Result<void> do_receive(std::string& buf, uint32_t timeout_ms) = 0;
#endif

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

    // Accessors for wire buffer — abstracts over std::string vs fixed buffer
    char* wire_data() {
#ifndef NOTE_NO_STD_STRING
        return wire_.data();
#else
        return wire_buf_;
#endif
    }
    size_t wire_capacity() const {
#ifndef NOTE_NO_STD_STRING
        return wire_.size();
#else
        return wire_cap_;
#endif
    }

    /// Prepare the wire buffer from a JSON request string.
    /// Default: copies request and adds CRC if enabled.
    /// Override to append a protocol-specific line terminator (e.g. I2C \n).
    virtual void prepare_wire(string_view request) {
#ifndef NOTE_NO_STD_STRING
        wire_.resize(request.size() + transport::detail::kCrcOverhead);
        memcpy(wire_.data(), request.data(), request.size());
#else
        if (request.size() + transport::detail::kCrcOverhead <= wire_cap_) {
            memcpy(wire_buf_, request.data(), request.size());
        }
#endif
        wire_len_ = request.size();
        if (crc_enabled_) {
            ++crc_seq_;
            wire_len_ = transport::detail::crc_add(
                wire_data(), wire_len_, wire_capacity(), crc_seq_);
        }
    }

    // ── Policy access — subclasses provide ────────────────────────────────

    virtual uint32_t max_retries() const = 0;
    virtual uint32_t retry_delay_ms() const = 0;
    virtual void delay(uint32_t ms) = 0;

    /// Write the protocol line terminator via do_write().
    /// Default: \r\n (serial). Override for I2C (\n).
    virtual bool write_line_terminator() {
        const uint8_t crlf[] = {'\r', '\n'};
        return do_write(crlf, 2);
    }

    /// Type-erased build function: void(*)(JsonBuilder&, void* ctx).
    /// Avoids per-lambda template instantiation of stream_request.
    using BuildFnPtr = void(*)(JsonBuilder&, void*);

    /// Stream a JSON request to the transport via do_write().
    /// Non-template: one copy shared by all callers.
    bool stream_request(BuildFnPtr build_fn, void* ctx) {
        RawWriter raw(*this);

#ifndef NOTE_NO_CRC
        if (crc_enabled_) {
            CrcWriter crc(raw);
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
            raw.write(suffix, pos);
        } else
#endif // NOTE_NO_CRC
        {
            StreamingJsonBuilder builder(raw);
            build_fn(builder, ctx);
            raw.write("}", 1);
        }

        if (!raw.ok) return false;

        return write_line_terminator();
    }

    /// Convenience: type-erase a callable into BuildFnPtr + void* ctx.
    template<typename BuildFn>
    bool stream_request(BuildFn& build_fn) {
        BuildFnPtr fn = [](JsonBuilder& b, void* p) {
            (*static_cast<BuildFn*>(p))(b);
        };
        return stream_request(fn, &build_fn);
    }

    /// Receive a response into the available buffer.
    /// Uses ext_buf_ if set, otherwise falls back to response_buf_.
    Result<string_view> receive_response(uint32_t timeout_ms) {
        if (ext_buf_) {
            return receive_into(ext_buf_, ext_buf_size_, timeout_ms);
        }
#ifndef NOTE_NO_STD_STRING
        response_buf_.clear();
        auto result = do_receive(response_buf_, timeout_ms);
        if (!result) return Unexpected(result.error());

        size_t rsp_len = response_buf_.size();
        if (transport::detail::crc_check_and_strip(
                response_buf_.data(), rsp_len, crc_seq_, crc_enabled_)) {
            return make_error(Error::ResponseLost, Cause::CrcMismatch,
                              "CRC mismatch");
        }
        response_buf_.resize(rsp_len);
        return string_view(response_buf_);
#else
        return make_error(Error::NotReady, "no receive buffer configured");
#endif
    }

    /// Receive and SAX-parse a response directly from the transport.
    /// CRC is verified via CrcAccumulator + CrcFieldSink (unless NOTE_NO_CRC).
    /// The caller's sink receives all events except "crc".
    Result<void> receive_streaming(JsonSink& sink, uint32_t timeout_ms) {
#ifndef NOTE_NO_CRC
        CrcFieldSink crc_sink(sink);
        transport::detail::CrcAccumulator crc;

        auto read_fn = [&](uint8_t* buf, size_t max, uint32_t t) -> Result<size_t> {
            auto r = do_read(buf, max, t);
            if (r) crc.feed(reinterpret_cast<const char*>(buf), *r);
            return r;
        };

        auto parse_err = sax_parse_streaming(read_fn, timeout_ms, crc_sink);
        if (!parse_err.empty())
            return make_error(Error::Json, parse_err);

        if (crc_sink.has_crc()) {
            if (crc_sink.seq() != crc_seq_ ||
                crc_sink.checksum() != crc.finalize_with_brace()) {
                return make_error(Error::ResponseLost, Cause::CrcMismatch,
                                  "CRC mismatch");
            }
            crc_enabled_ = true;
        } else if (crc_enabled_) {
            return make_error(Error::ResponseLost, Cause::CrcMismatch,
                              "expected CRC");
        }
#else
        auto read_fn = [&](uint8_t* buf, size_t max, uint32_t t) -> Result<size_t> {
            return do_read(buf, max, t);
        };

        auto parse_err = sax_parse_streaming(read_fn, timeout_ms, sink);
        if (!parse_err.empty())
            return make_error(Error::Json, parse_err);
#endif // NOTE_NO_CRC

        return {};
    }

    // ── Shared state ──────────────────────────────────────────────────────

    bool initialized_ = false;
    bool crc_enabled_ = false;
    uint16_t crc_seq_ = 0;

#ifndef NOTE_NO_STD_STRING
    std::string wire_;          // outbound buffer (sized with CRC headroom)
#else
    char* wire_buf_ = nullptr;  // fixed outbound buffer (set by subclass)
    size_t wire_cap_ = 0;
#endif
    size_t wire_len_ = 0;      // actual wire content length
#ifndef NOTE_NO_STD_STRING
    std::string response_buf_;  // inbound buffer (legacy path)
#endif

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

        // CRC check (in-place)
        if (transport::detail::crc_check_and_strip(buf, pos, crc_seq_, crc_enabled_)) {
            return make_error(Error::ResponseLost, Cause::CrcMismatch, "CRC mismatch");
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

} // namespace note
