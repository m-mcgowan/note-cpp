#pragma once

#include "allocator.hpp"
#include "binary_request.hpp"
#include "debug.hpp"
#include "owned_buffer.hpp"
#include "json.hpp"
#include "md5.hpp"
#include "retry.hpp"
#include "retry_policy.hpp"
#include "safety.hpp"
#include "span.hpp"
#include "streaming_transport.hpp"
#include "string_pool.hpp"
#include "struct_sink.hpp"
#include "transport.hpp"
#include "transport/cobs.hpp"

#include <optional>
#include <type_traits>

namespace note {

namespace detail {
    template<typename T, typename = void>
    struct has_intern_strings : std::false_type {};
    template<typename T>
    struct has_intern_strings<T, std::void_t<decltype(std::declval<T>().intern_strings(std::declval<StringPool&>()))>> : std::true_type {};

    template<typename T, typename = void>
    struct has_sink : std::false_type {};
    template<typename T>
    struct has_sink<T, std::void_t<typename T::Sink>> : std::true_type {};

    template<typename T, typename = void>
    struct has_body_factory : std::false_type {};
    template<typename T>
    struct has_body_factory<T, std::void_t<decltype(T::body_handler_factory_)>> : std::true_type {};

    template<typename T, typename = void>
    struct has_set_body_handler : std::false_type {};
    template<typename T>
    struct has_set_body_handler<T, std::void_t<decltype(std::declval<T>().set_body_handler(std::declval<BodyHandler>()))>> : std::true_type {};
}


// Specialization for void responses (endpoints that return empty {} on success).
// Still holds a reader to keep notecard error message string_views alive.
template<>
class ApiResult<void> {
    std::optional<ErrorInfo> err_;
#ifndef NOTE_NO_BUFFERED
    std::unique_ptr<JsonReader> reader_;
#endif
public:
    ApiResult() = default;
    ApiResult(ErrorInfo e) : err_(std::move(e)) {}
#ifndef NOTE_NO_BUFFERED
    ApiResult(ErrorInfo e, std::unique_ptr<JsonReader> reader)
        : err_(std::move(e)), reader_(std::move(reader)) {}
#endif
#if defined(__cpp_lib_expected) && __cpp_lib_expected >= 202202L
    ApiResult(Unexpected e) : err_(std::move(e).error()) {}
#else
    ApiResult(Unexpected e) : err_(std::move(e).value()) {}
#endif

    explicit operator bool() const { return !err_.has_value(); }
    bool has_value() const { return !err_.has_value(); }

    const ErrorInfo& error() const { return *err_; }
};

#ifndef NOTE_MINIMAL
class Notecard {
public:
    Notecard() = default;

    Notecard(JsonBackend& backend, IBufferedTransport& transport)
        : backend_(&backend)
        , transport_(&transport)
    {}

    /// Streaming-only: no JsonBackend, no ITransport vtable overhead.
    /// Requests streamed via StreamingJsonBuilder, responses SAX-parsed via Sink.
    /// Allocator defaults to heap (operator new/delete).
    Notecard(IStreamingTransport& transport, Allocator alloc = {})
        : streaming_transport_(&transport)
        , alloc_(alloc)
    {}

    // Configure an allocator for response string interning.
    // When set, execute() copies response string_view fields into the
    // allocator's backing store (e.g. a MonotonicArena) so they survive
    // transport buffer reuse. The parsing strategy is still dictated by
    // the backend (tree-parse or SAX).
    void set_allocator(Allocator alloc) { alloc_ = alloc; }
    void clear_allocator() { alloc_.reset(); }

    // Configure the working buffer for COBS encode/decode in binary transfers.
    // Set once at startup; all binary execute() calls use it automatically.
    // If not set, a NOTE_COBS_BLOCK_SIZE stack buffer is used per call.
    void set_cobs_buffer(byte_span buf) { cobs_buf_ = buf; }
    void set_cobs_buffer(uint8_t* buf, size_t len) { cobs_buf_ = {buf, len}; }
    template<size_t N>
    void set_cobs_buffer(uint8_t (&buf)[N]) { cobs_buf_ = buf; }

    // Configure the MD5 provider for binary transfer integrity checks.
    // Defaults to PlatformMd5 (MbedTlsMd5 when available, else SoftwareMd5).
    // Pass a custom implementation to use hardware-accelerated MD5.
    void set_md5_provider(Md5Provider& provider) { md5_ = &provider; }

    // Execute a typed, generated request.
    // RequestT must provide:
    //   static constexpr string_view notecard_request;
    //   static constexpr bool supports_cmd;
    //   static constexpr Safety safety;
    //   using Response = ...;
    //   void build(JsonBuilder&) const;
    template<typename RequestT>
    ApiResult<typename RequestT::Response> execute(const RequestT& req) {
        using Rsp = typename RequestT::Response;

        debug_timing(debug_, TimingEvent::TransactionBegin, RequestT::notecard_request);

        if (!transport_ && !streaming_transport_) {
            debug_timing(debug_, TimingEvent::TransactionEnd, RequestT::notecard_request);
            return Unexpected(make_error(Error::NotReady, NOTE_ERR("no transport configured")));
        }

        // Full streaming path: SAX-parse response directly from transport.
        if constexpr (std::is_void_v<Rsp> || detail::has_sink<Rsp>::value) {
            if (streaming_transport_ && alloc_.has_value()) {
                auto build = [&](JsonBuilder& b) { req.build(b); };
                BuildFn build_fn = [](JsonBuilder& b, void* p) {
                    (*static_cast<decltype(build)*>(p))(b);
                };
                RequestFrame frame{build_fn, &build, RequestT::notecard_request,
                                   request_ids_enabled_ ? next_request_id_++ : 0};

                if constexpr (std::is_void_v<Rsp>) {
                    JsonSink null_sink;
                    auto ei = streaming_execute(frame, null_sink, RequestT::safety,
                                                nullptr, nullptr);
                    debug_timing(debug_, TimingEvent::TransactionEnd, RequestT::notecard_request);
                    if (ei.code != Error{}) return ApiResult<void>(ei);
                    return ApiResult<void>{};
                } else {
                    Rsp rsp{};
                    auto reset_rsp = [](void* p) { *static_cast<Rsp*>(p) = Rsp{}; };
                    alignas(body_sink_storage_align) char body_storage[body_sink_storage_size];
                    auto ei = streaming_execute_typed<typename Rsp::Sink>(
                        frame, rsp, RequestT::safety, reset_rsp, &rsp,
                        [&](StringPool& pool) -> BodyHandler {
                            if constexpr (detail::has_body_factory<RequestT>::value) {
                                if (req.body_handler_factory_) {
                                    return req.body_handler_factory_(req.body_ptr_, pool, body_storage);
                                }
                            }
                            return {};
                        });
                    debug_timing(debug_, TimingEvent::TransactionEnd, RequestT::notecard_request);
                    if (ei.code != Error{}) return ApiResult<Rsp>(ei);
                    return ApiResult<Rsp>(std::move(rsp));
                }
            }
        }

        // Buffered fallback: requires a JsonBackend + buffered transport.
#ifndef NOTE_NO_BUFFERED
        if (backend_) {
            const uint32_t req_id = request_ids_enabled_ ? next_request_id_++ : 0;
            auto attempt = [&]() -> ApiResult<Rsp> {
                return execute_buffered(req, req_id);
            };
            auto reset = [&]() { transport_->reset(); };

            auto result = retry_transaction<ApiResult<Rsp>>(
                *transport_, timing_, RequestT::safety, retry_policy_,
                attempt, reset);
            debug_timing(debug_, TimingEvent::TransactionEnd, RequestT::notecard_request);
            return result;
        }
#endif
        debug_timing(debug_, TimingEvent::TransactionEnd, RequestT::notecard_request);
        return Unexpected(make_error(Error::NotReady, NOTE_ERR("no backend or streaming transport configured")));
    }

    // ── Binary transfer support ────────────────────────────────────────────
    //
    // Requests with .data() or .into() carry binary buffers. execute()
    // on a non-const request checks for attached buffers and handles
    // COBS encode/decode transparently.

    /// Execute a mutable request — checks for attached binary buffers.
    template<typename RequestT>
    ApiResult<typename RequestT::Response> execute(RequestT& req) {
        if constexpr (detail::has_binary_src<RequestT>::value) {
            if (req.has_binary_data()) {
                return do_binary_send(req);
            }
        }
        if constexpr (detail::has_binary_dst<RequestT>::value) {
            if (req.has_binary_buffer()) {
                return do_binary_receive(req);
            }
        }
        return execute(static_cast<const RequestT&>(req));
    }

    // Execute with an explicit allocator (one-off string interning).
    template<typename RequestT>
    ApiResult<typename RequestT::Response> execute(const RequestT& req, Allocator alloc) {
        auto saved = alloc_;
        alloc_ = alloc;
        auto result = execute(req);
        alloc_ = saved;
        return result;
    }

#if !defined(NOTE_NO_STD_STRING) && !defined(NOTE_NO_STD_FUNCTION)
    // Ad-hoc request with a builder callback.
    // Requires std::function and a buffered transport + backend.
    Result<std::unique_ptr<JsonReader>> request(
            string_view req_type,
            std::function<void(JsonBuilder&)> build_fn = {}) {
        if (!transport_ || !backend_)
            return make_error(Error::NotReady, NOTE_ERR("no buffered transport configured"));

        auto& builder = backend_->get_builder();
        builder.add("req", req_type);
        if (build_fn) build_fn(builder);
        auto rsp = transport_->transact(builder.to_view(), default_timeout_ms_);
        if (!rsp) return Unexpected(rsp.error());

        auto reader = backend_->parse_response(*rsp);
        if (reader->has_error()) {
            return make_error(Error::Json, reader->get_error());
        }
        return Result<std::unique_ptr<JsonReader>>(std::move(reader));
    }

#endif // !NOTE_NO_STD_STRING && !NOTE_NO_STD_FUNCTION

    /// Type-erased send (fire-and-forget). Used by generated command() methods
    /// via send_fn_ — a single shared function pointer for all request types.
    Result<void> send_command(BuildFn build_fn, void* ctx) {
        enforce_timing();
        Result<void> result;
        if (streaming_transport_)
            result = streaming_transport_->send(build_fn, ctx);
#ifndef NOTE_NO_BUFFERED
        else if (transport_) {
            auto& builder = backend_->get_builder();
            build_fn(builder, ctx);
            result = transport_->send(builder.to_view());
        }
#endif
        else
            return make_error(Error::NotReady, NOTE_ERR("no transport configured"));
        record_timing();
        return result;
    }

    template<typename RequestT>
    Result<void> command_typed(const RequestT& req) {
        enforce_timing();
        Result<void> result;
        if (streaming_transport_) {
            auto build = [&](JsonBuilder& b) {
                b.add("cmd", RequestT::notecard_request);
                req.build(b);
            };
            BuildFn fn = [](JsonBuilder& b, void* p) {
                (*static_cast<decltype(build)*>(p))(b);
            };
            result = streaming_transport_->send(fn, &build);
        } else if (transport_) {
            auto& builder = backend_->get_builder();
            builder.add("cmd", RequestT::notecard_request);
            req.build(builder);
            result = transport_->send(builder.to_view());
        } else {
            return make_error(Error::NotReady, NOTE_ERR("no transport configured"));
        }
        record_timing();
        return result;
    }

#if !defined(NOTE_NO_STD_STRING) && !defined(NOTE_NO_STD_FUNCTION)
    // Fire-and-forget command with builder callback.
    // Requires std::function.
    Result<void> command(string_view cmd_type,
                         std::function<void(JsonBuilder&)> build_fn = {}) {
        if (streaming_transport_) {
            return streaming_transport_->send([&](JsonBuilder& b) {
                b.add("cmd", cmd_type);
                if (build_fn) build_fn(b);
            });
        }
        if (!transport_) return make_error(Error::NotReady, NOTE_ERR("no transport configured"));

        auto& builder = backend_->get_builder();
        builder.add("cmd", cmd_type);
        if (build_fn) build_fn(builder);
        return transport_->send(builder.to_view());
    }
#endif // !NOTE_NO_STD_STRING && !NOTE_NO_STD_FUNCTION

    /// Validated JSON passthrough — returns an OwnedBuffer that the caller owns.
    /// The buffer is freed when it goes out of scope. No dangling views.
    /// Assumes NonIdempotent safety (only retries on SendFailed).
    Result<OwnedBuffer> transact(string_view json) {
        if (!validate_json_envelope(json))
            return make_error(Error::Json, NOTE_ERR("malformed JSON"));

        // Buffered path with retry.
        if (transport_) {
            auto attempt = [&]() -> Result<OwnedBuffer> {
                auto rv = transport_->transact(json, default_timeout_ms_);
                if (!rv) return Unexpected(rv.error());
                auto buf = OwnedBuffer::create(alloc_value(), rv->size() + 1);
                if (!buf) return make_error(Error::Overflow, NOTE_ERR("alloc failed"));
                buf.append(rv->data(), rv->size());
                buf.null_terminate();
                return std::move(buf);
            };
            auto reset = [&]() { transport_->reset(); };
            return retry_transaction<Result<OwnedBuffer>>(
                *transport_, timing_, Safety::NonIdempotent, retry_policy_,
                attempt, reset);
        }

        // Streaming path with retry.
        if (streaming_transport_) {
            auto attempt = [&]() -> Result<OwnedBuffer> {
                auto* st = static_cast<StreamingTransport*>(streaming_transport_);
                auto send_rv = st->send_raw(json);
                if (!send_rv) return Unexpected(send_rv.error());

                auto buf = OwnedBuffer::create(alloc_value(), 1024);
                if (!buf) return make_error(Error::Overflow, NOTE_ERR("alloc failed"));

                for (;;) {
                    uint8_t byte;
                    auto rv = st->read(&byte, 1, default_timeout_ms_);
                    if (!rv) return Unexpected(rv.error());
                    if (*rv == 0) return make_error(Error::ResponseLost, Cause::Timeout, NOTE_ERR("response timeout"));
                    if (byte == '\n') break;
                    if (byte == '\r') continue;
                    if (!buf.append(static_cast<char>(byte)))
                        return make_error(Error::Overflow, NOTE_ERR("response exceeds available memory"));
                }
                buf.null_terminate();
                return std::move(buf);
            };
            auto reset = [&]() { streaming_transport_->reset(); };
            return retry_transaction<Result<OwnedBuffer>>(
                *streaming_transport_, timing_, Safety::NonIdempotent, retry_policy_,
                attempt, reset);
        }

        return make_error(Error::NotReady, NOTE_ERR("no transport configured"));
    }

    /// Validated JSON passthrough — caller-provided buffer variant.
    /// The response is written into buf and returned as string_view.
    /// Assumes NonIdempotent safety (only retries on SendFailed).
    Result<string_view> transact(string_view json, span<char> buf) {
        if (!validate_json_envelope(json))
            return make_error(Error::Json, NOTE_ERR("malformed JSON"));
        if (transport_) {
            auto attempt = [&]() -> Result<string_view> {
                auto rv = transport_->transact(json, default_timeout_ms_);
                if (!rv) return rv;
                auto rsp = *rv;
                if (rsp.size() >= buf.size())
                    return make_error(Error::Overflow, NOTE_ERR("response exceeds buffer"));
                std::memcpy(buf.data(), rsp.data(), rsp.size());
                buf[rsp.size()] = '\0';
                return string_view(buf.data(), rsp.size());
            };
            auto reset = [&]() { transport_->reset(); };
            return retry_transaction<Result<string_view>>(
                *transport_, timing_, Safety::NonIdempotent, retry_policy_,
                attempt, reset);
        }
        if (streaming_transport_) {
            auto attempt = [&]() -> Result<string_view> {
                return streaming_transact_raw(json, buf);
            };
            auto reset = [&]() { streaming_transport_->reset(); };
            return retry_transaction<Result<string_view>>(
                *streaming_transport_, timing_, Safety::NonIdempotent, retry_policy_,
                attempt, reset);
        }
        return make_error(Error::NotReady, NOTE_ERR("no transport configured"));
    }

    /// Validated JSON fire-and-forget — send pre-formatted JSON, no response.
    /// Inter-transaction timing enforced, but no retry (no response to check).
    Result<void> send(string_view json) {
        if (!validate_json_envelope(json))
            return make_error(Error::Json, NOTE_ERR("malformed JSON"));
        enforce_timing();
        Result<void> result;
        if (transport_)
            result = transport_->send(json);
        else if (streaming_transport_)
            result = streaming_send_raw(json);
        else
            return make_error(Error::NotReady, NOTE_ERR("no transport configured"));
        record_timing();
        return result;
    }

    void set_default_timeout(uint32_t ms) { default_timeout_ms_ = ms; }
    uint32_t default_timeout() const { return default_timeout_ms_; }

    void set_retry_policy(RetryPolicy policy) { retry_policy_ = policy; }
    const RetryPolicy& retry_policy() const { return retry_policy_; }

    void set_inter_transaction_gap(uint32_t ms) { timing_.min_gap_ms = ms; }
    uint32_t inter_transaction_gap() const { return timing_.min_gap_ms; }

    /// Enable/disable auto-incrementing request IDs in JSON output.
    /// Default: enabled. Disable in tests that check exact wire format.
    void set_request_ids(bool enabled) { request_ids_enabled_ = enabled; }

    /// Set a debug listener for observability (wire data, timing, memory).
    /// Pass a default-constructed DebugListener or call clear_debug() to disable.
    void set_debug(DebugListener d) {
        debug_ = d;
        if (streaming_transport_)
            streaming_transport_->set_debug(d);
    }

    /// Disable all debug callbacks.
    void clear_debug() { set_debug({}); }

    /// Access the current debug listener.
    const DebugListener& debug() const { return debug_; }

    /// Access the underlying transport.
    IBufferedTransport& transport() { return *transport_; }

    JsonBackend& backend() { return *backend_; }

private:
    template<typename RequestT>
    ApiResult<typename RequestT::Response> do_binary_send(RequestT& req) {
        using Rsp = typename RequestT::Response;
        auto src = req.binary_src_;

        // Pre-flight: check space and auto-reset if offset==0.
        if (req.binary_verify_) {
            // Reset on first segment (offset not set or == 0)
            if (!req.offset || *req.offset == 0) {
                auto clear = binary_control(R"({"req":"card.binary","delete":true})");
                if (!clear)
                    return ApiResult<Rsp>(
                        ErrorInfo{Error::SendFailed, Cause::Unspecified, NOTE_ERR("binary reset failed")});
            }
            auto status = binary_control(R"({"req":"card.binary"})");
            if (!status)
                return ApiResult<Rsp>(
                    ErrorInfo{Error::SendFailed, Cause::Unspecified, NOTE_ERR("binary status query failed")});
            auto max_bytes = binary_response_int(*status, "max");
            if (max_bytes <= 0)
                return ApiResult<Rsp>(
                    ErrorInfo{Error::Overflow, Cause::Unspecified, NOTE_ERR("binary store not available")});
            if (static_cast<int32_t>(src.size()) > max_bytes)
                return ApiResult<Rsp>(
                    ErrorInfo{Error::Overflow, Cause::Unspecified, NOTE_ERR("data exceeds binary store capacity")});
        }

        req.cobs = static_cast<int32_t>(cobs_encoded_length(src.data(), src.size()));
        Md5Hex md5_hex;
        if (md5_) {
            md5_hex = md5_->compute(src.data(), src.size());
            req.status = md5_hex;  // string_view into md5_hex.buf — stack lifetime
        }

        auto result = execute(static_cast<const RequestT&>(req));
        if (!result) return result;

        // Stream COBS-encoded blocks via transport write().
        CobsEncoder encoder;
        bool tx_ok = true;
        encoder.encode(src.data(), src.size(), [&](const uint8_t* block, size_t n) {
            if (tx_ok) tx_ok = !!binary_write(block, n);
        });
        if (tx_ok) {
            uint8_t eop = cobs_eop;
            tx_ok = !!binary_write(&eop, 1);
        }
        if (!tx_ok) {
            binary_io_reset();
            return ApiResult<Rsp>(
                ErrorInfo{Error::SendFailed, Cause::HalError, NOTE_ERR("binary transmit failed")});
        }

        // Post-transmit verification: query card.binary status and confirm
        // the Notecard's stored MD5 matches what we sent.
        if (req.binary_verify_ && !md5_hex.empty()) {
            auto verify = binary_control(R"({"req":"card.binary"})");
            if (!verify)
                return ApiResult<Rsp>(
                    ErrorInfo{Error::ResponseLost, Cause::Unspecified, NOTE_ERR("binary verify query failed")});
            auto stored_md5 = binary_response_string(*verify, "status");
            if (!stored_md5.empty() && stored_md5 != string_view(md5_hex))
                return ApiResult<Rsp>(
                    ErrorInfo{Error::ResponseLost, Cause::CrcMismatch, NOTE_ERR("binary verify: MD5 mismatch")});
        }

        return result;
    }

    template<typename RequestT>
    ApiResult<typename RequestT::Response> do_binary_receive(RequestT& req) {
        using Rsp = typename RequestT::Response;
        auto dst = req.binary_dst_;
        auto result = execute(static_cast<const RequestT&>(req));
        if (!result) return result;

        // Stream COBS-encoded bytes via transport read(), decode incrementally.
        CobsDecoder decoder;
        size_t decoded = 0;
        auto decode_sink = [&](const uint8_t* data, size_t n) {
            size_t copy = (decoded + n <= dst.size()) ? n : (dst.size() - decoded);
            memcpy(dst.data() + decoded, data, copy);
            decoded += copy;
        };

        uint8_t chunk[64];
        bool eop_seen = false;
        while (!eop_seen) {
            auto r = binary_read(chunk, sizeof(chunk), default_timeout_ms_);
            if (!r) {
                binary_io_reset();
                return ApiResult<Rsp>(
                    ErrorInfo{Error::ResponseLost, Cause::Timeout, NOTE_ERR("binary receive timeout")});
            }
            size_t n = *r;
            for (size_t i = 0; i < n; ++i) {
                if (chunk[i] == cobs_eop) { eop_seen = true; n = i + 1; break; }
            }
            decoder.feed(chunk, n, decode_sink);
        }
        decoder.flush(decode_sink);

        // MD5 verify: compare decoded bytes against expected hash from response.
        string_view expected_md5 = result.status;
        if (md5_ && !expected_md5.empty()) {
            auto actual = md5_->compute(dst.data(), decoded);
            if (actual != expected_md5) {
                binary_io_reset();
                return ApiResult<Rsp>(
                    ErrorInfo{Error::ResponseLost, Cause::CrcMismatch, "MD5 mismatch"});
            }
        }

        return result;
    }

    // ── Binary transfer helpers ────────────────────────────────────────

    /// Binary I/O: write bytes to whichever transport is available.
    Result<void> binary_write(const uint8_t* data, size_t len) {
        if (streaming_transport_) return streaming_transport_->write(data, len);
        if (transport_) return transport_->write(data, len);
        return make_error(Error::NotReady, NOTE_ERR("no transport for binary I/O"));
    }
    /// Binary I/O: read bytes from whichever transport is available.
    Result<size_t> binary_read(uint8_t* buf, size_t max_len, uint32_t timeout_ms) {
        if (streaming_transport_) return streaming_transport_->read(buf, max_len, timeout_ms);
        if (transport_) return transport_->read(buf, max_len, timeout_ms);
        return make_error(Error::NotReady, NOTE_ERR("no transport for binary I/O"));
    }
    void binary_io_reset() {
        if (streaming_transport_) streaming_transport_->reset();
        else if (transport_) transport_->reset();
    }

    /// Send a raw JSON control command for binary transfer and return
    /// the response as a string_view. Uses streaming transact_raw when
    /// available, falls back to buffered transport.
    Result<string_view> binary_control(string_view json) {
        if (streaming_transport_) {
            return streaming_transact_raw(json, span<char>(binary_ctrl_buf_, sizeof(binary_ctrl_buf_)));
        }
#ifndef NOTE_NO_BUFFERED
        if (transport_ && backend_) {
            return transport_->transact(json, default_timeout_ms_);
        }
#endif
        return make_error(Error::NotReady, NOTE_ERR("no transport for binary control"));
    }

    /// Extract an integer field from a raw JSON response (for binary control).
    static int32_t binary_response_int(string_view json, string_view key) {
        // Minimal parse: find "key":NUMBER in the JSON
        JsonSink null_sink;
        struct IntCapture : JsonSink {
            string_view target_key;
            int32_t value = 0;
            void on_number(string_view k, string_view raw) override {
                if (k == target_key) value = parse_int(raw);
            }
        } capture;
        capture.target_key = key;
        sax_parse(json, capture);
        return capture.value;
    }

    /// Extract a string field from a raw JSON response (for binary control).
    static string_view binary_response_string(string_view json, string_view key) {
        // Returns a view into the json buffer (valid as long as json is).
        struct StringCapture : JsonSink {
            string_view target_key;
            string_view value{};
            void on_string(string_view k, string_view v) override {
                if (k == target_key) value = v;
            }
        } capture;
        capture.target_key = key;
        sax_parse(json, capture);
        return capture.value;
    }

    /// Validate that a string is well-formed JSON using the SAX parser.
    static bool validate_json_envelope(string_view json) {
        JsonSink null_sink;
        auto err = sax_parse(json, null_sink);
        return err.empty();
    }

    /// Raw passthrough on the streaming transport — transmit bytes, read response.
    Result<string_view> streaming_transact_raw(string_view json, span<char> buf) {
        auto* st = static_cast<StreamingTransport*>(streaming_transport_);
        return st->transact_raw(json, buf.data(), buf.size(), default_timeout_ms_);
    }

    /// Raw fire-and-forget on the streaming transport.
    Result<void> streaming_send_raw(string_view json) {
        auto* st = static_cast<StreamingTransport*>(streaming_transport_);
        return st->send_raw(json);
    }

    /// Buffered execute: build JSON via backend, transact, parse response.
    /// Separated from execute() so LTO can eliminate it when backend_ is null.
    template<typename RequestT>
    ApiResult<typename RequestT::Response> execute_buffered(const RequestT& req, uint32_t req_id = 0) {
        using Rsp = typename RequestT::Response;

        Result<string_view> rsp = make_error(Error::NotReady, "");
        {
            debug_timing(debug_, TimingEvent::BuildBegin, RequestT::notecard_request);
            auto& builder = backend_->get_builder();
            builder.add("req", RequestT::notecard_request);
            if (req_id) builder.add("id", static_cast<int32_t>(req_id));
            req.build(builder);
            auto req_json = builder.to_view();
            debug_timing(debug_, TimingEvent::BuildEnd, RequestT::notecard_request);
            debug_wire(debug_, req_json, WireDirection::Send);
            rsp = transport_->transact(req_json, default_timeout_ms_);
        }
        if (!rsp) return Unexpected(rsp.error());
        debug_wire(debug_, *rsp, WireDirection::Receive);

        auto& reader = backend_->get_reader(*rsp);
        if (reader.has_error()) {
            return make_error(Error::Json, "JSON parse error");
        }
        auto err = reader.get_error();
        if (!err.empty()) {
            if (alloc_.has_value()) {
                StringPool pool(*alloc_);
                ErrorInfo ei{Error::Notecard, Cause::Unspecified, pool.intern(err)};
                return ApiResult<Rsp>(std::move(ei));
            }
            auto owned = backend_->parse_response(*rsp);
            ErrorInfo ei{Error::Notecard, Cause::Unspecified, owned->get_error()};
            return ApiResult<Rsp>(std::move(ei), std::move(owned));
        }
        if constexpr (std::is_void_v<Rsp>) {
            return ApiResult<void>{};
        } else {
            ApiResult<Rsp> result(Rsp::parse(reader));
            if constexpr (detail::has_intern_strings<Rsp>::value) {
                if (alloc_.has_value()) {
                    StringPool pool(*alloc_);
                    result.intern_strings(pool);
                }
            }
            return result;
        }
    }

    // ── Request framing ─────────────────────────────────────────────────

    /// Bundles a type-erased build function with request name and ID.
    /// Passed to streaming_execute / framed_build.
    struct RequestFrame {
        BuildFn inner;
        void* inner_ctx;
        string_view request_name;
        uint32_t req_id;
    };

    /// Non-template build function: prepends "req" and optional "id"
    /// before delegating to the per-endpoint builder.
    static void framed_build(JsonBuilder& b, void* p) {
        auto& f = *static_cast<RequestFrame*>(p);
        b.add("req", f.request_name);
        if (f.req_id) b.add("id", static_cast<int32_t>(f.req_id));
        f.inner(b, f.inner_ctx);
    }

    // ── Streaming execute (non-template) ────────────────────────────────

    /// Single streaming attempt: transact + error capture.
    /// Pool is used for error message interning only.
    ErrorInfo streaming_attempt(RequestFrame& frame, JsonSink& sink) {
#ifndef NOTE_NO_STD_STRING
        if (debug_.on_wire) {
            struct SizingWriter : JsonWriter {
                using JsonWriter::write;
                std::string buf;
                bool write(const char* data, size_t len) override {
                    buf.append(data, len);
                    return true;
                }
            } sizer;
            StreamingJsonBuilder sizing_builder(sizer);
            framed_build(sizing_builder, &frame);
            sizer.buf += '}';
            debug_wire(debug_, string_view(sizer.buf.data(), sizer.buf.size()), WireDirection::Send);
        }
#endif // NOTE_NO_STD_STRING

        ErrorCaptureSink err_sink(sink);
        auto rv = streaming_transport_->transact(
            framed_build, &frame, err_sink, default_timeout_ms_);
        if (!rv) return rv.error();
        auto err = err_sink.captured_error();
        if (!err.empty()) {
            // Intern via the Notecard's allocator so the message outlives
            // this scope. StringPool is lightweight (no destructor cost).
            StringPool pool(*alloc_);
            return ErrorInfo{Error::Notecard, Cause::Unspecified, pool.intern(err)};
        }
        return {};
    }

    /// Streaming execute with retry — non-template.
    /// Calls sink.reset() + reset_fn between retries.
    /// reset_fn/reset_ctx allow the template caller to zero the Response.
    ErrorInfo streaming_execute(RequestFrame& frame, JsonSink& sink,
                                Safety safety,
                                void (*reset_fn)(void*), void* reset_ctx) {
        auto attempt = [&]() -> Result<void> {
            auto ei = streaming_attempt(frame, sink);
            if (ei.code != Error{}) return Unexpected(ei);
            return {};
        };
        auto reset = [&]() {
            streaming_transport_->reset();
            sink.reset();
            if (reset_fn) reset_fn(reset_ctx);
        };
        auto result = retry_transaction<Result<void>>(
            *streaming_transport_, timing_, safety, retry_policy_,
            attempt, reset);
        if (!result) return result.error();
        return {};
    }

    /// Typed streaming execute — thin template that constructs the
    /// per-type Sink and delegates to the non-template streaming_execute.
    template<typename SinkT, typename RspT>
    ErrorInfo streaming_execute_typed(RequestFrame& frame, RspT& rsp,
                                      Safety safety,
                                      void (*reset_fn)(void*), void* reset_ctx) {
        StringPool pool(*alloc_);
        SinkT response_sink(rsp, pool);
        JsonSinkAdapter<SinkT> virtual_sink(response_sink);
        return streaming_execute(frame, virtual_sink, safety, reset_fn, reset_ctx);
    }

    /// Typed streaming execute with body handler factory.
    /// The factory is called with the StringPool so StructSink can intern strings.
    template<typename SinkT, typename RspT, typename BodyFactoryFn>
    ErrorInfo streaming_execute_typed(RequestFrame& frame, RspT& rsp,
                                      Safety safety,
                                      void (*reset_fn)(void*), void* reset_ctx,
                                      BodyFactoryFn&& body_factory) {
        StringPool pool(*alloc_);
        SinkT response_sink(rsp, pool);
        if constexpr (detail::has_set_body_handler<SinkT>::value) {
            auto bh = body_factory(pool);
            if (bh) response_sink.set_body_handler(bh);
        }
        JsonSinkAdapter<SinkT> virtual_sink(response_sink);
        return streaming_execute(frame, virtual_sink, safety, reset_fn, reset_ctx);
    }

    /// Streaming execute core — template on transport to support both
    /// virtual (IStreamingTransport*) and concrete (StaticNotecard) dispatch.
    /// When called with a pointer, the compiler generates one shared copy.
    /// When called with a concrete ref, it devirtualizes on modern GCC.
    template<typename Transport>
    static ErrorInfo execute_streaming(Transport& t, uint32_t timeout_ms,
                                       BuildFn build_fn, void* ctx,
                                       JsonSink& inner_sink, StringPool& pool,
                                       const DebugListener& debug = {}) {
        // Emit wire-send event: build the JSON into a temporary buffer for debug.
        // Only when a wire listener is set — zero overhead otherwise.
#ifndef NOTE_NO_STD_STRING
        if (debug.on_wire) {
            // Build into a sizing pass to get the JSON for debug output.
            // This duplicates the build but only when debug is active.
            struct SizingWriter : JsonWriter {
                using JsonWriter::write;
                std::string buf;
                bool write(const char* data, size_t len) override {
                    buf.append(data, len);
                    return true;
                }
            } sizer;
            StreamingJsonBuilder sizing_builder(sizer);
            build_fn(sizing_builder, ctx);
            sizer.buf += '}';
            debug_wire(debug, string_view(sizer.buf.data(), sizer.buf.size()), WireDirection::Send);
        }
#endif // NOTE_NO_STD_STRING

        ErrorCaptureSink err_sink(inner_sink);
        auto rv = t.transact(build_fn, ctx, err_sink, timeout_ms);
        if (!rv) return rv.error();
        auto err = err_sink.captured_error();
        if (!err.empty())
            return ErrorInfo{Error::Notecard, Cause::Unspecified, pool.intern(err)};
        return {};
    }

    /// Enforce inter-transaction gap using whichever transport is active.
    void enforce_timing() {
        if (!timing_.has_previous) return;
        if (streaming_transport_) {
            uint32_t elapsed = streaming_transport_->millis() - timing_.last_transaction_end_ms;
            if (elapsed < timing_.min_gap_ms)
                streaming_transport_->delay(timing_.min_gap_ms - elapsed);
        } else if (transport_) {
            uint32_t elapsed = transport_->millis() - timing_.last_transaction_end_ms;
            if (elapsed < timing_.min_gap_ms)
                transport_->delay(timing_.min_gap_ms - elapsed);
        }
    }

    /// Record that a transaction just completed.
    void record_timing() {
        if (streaming_transport_)
            timing_.last_transaction_end_ms = streaming_transport_->millis();
        else if (transport_)
            timing_.last_transaction_end_ms = transport_->millis();
        timing_.has_previous = true;
    }

    Allocator alloc_value() const { return alloc_.value_or(Allocator{}); }

    JsonBackend* backend_ = nullptr;
    IBufferedTransport* transport_ = nullptr;
    IStreamingTransport* streaming_transport_ = nullptr;
    uint32_t default_timeout_ms_ = 10000;
    std::optional<Allocator> alloc_;
    DebugListener debug_{};
    byte_span cobs_buf_{};          // optional external COBS working buffer
    char binary_ctrl_buf_[256]{};   // buffer for binary control command responses
    RetryPolicy retry_policy_{};
    TransactionTiming timing_{};
    uint32_t next_request_id_ = 1;
    bool request_ids_enabled_ = true;
#ifndef NOTE_NO_MD5
    PlatformMd5 platform_md5_{};    // default MD5 implementation
    Md5Provider* md5_ = &platform_md5_;
#else
    Md5Provider* md5_ = nullptr;
#endif
};
#endif // !NOTE_MINIMAL

} // namespace note
