#pragma once

#include "allocator.hpp"
#include "binary_request.hpp"
#include "json.hpp"
#include "md5.hpp"
#include "safety.hpp"
#include "span.hpp"
#include "streaming_transport.hpp"
#include "string_pool.hpp"
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
}


// Specialization for void responses (endpoints that return empty {} on success).
// Still holds a reader to keep notecard error message string_views alive.
template<>
class ApiResult<void> {
    std::optional<ErrorInfo> err_;
    std::unique_ptr<JsonReader> reader_;
public:
    ApiResult() = default;
    ApiResult(ErrorInfo e) : err_(std::move(e)) {}
    ApiResult(ErrorInfo e, std::unique_ptr<JsonReader> reader)
        : err_(std::move(e)), reader_(std::move(reader)) {}
#if defined(__cpp_lib_expected) && __cpp_lib_expected >= 202202L
    ApiResult(Unexpected e) : err_(std::move(e).error()) {}
#else
    ApiResult(Unexpected e) : err_(std::move(e).value()) {}
#endif

    explicit operator bool() const { return !err_.has_value(); }
    bool has_value() const { return !err_.has_value(); }

    const ErrorInfo& error() const { return *err_; }
};

class Notecard {
public:
    Notecard() = default;

    Notecard(JsonBackend& backend, IBufferedTransport& transport)
        : backend_(&backend)
        , transport_(&transport)
    {}

    /// Streaming-only: no JsonBackend, no ITransport vtable overhead.
    /// Requests streamed via StreamingJsonBuilder, responses SAX-parsed via Sink.
    Notecard(IStreamingTransport& transport, Allocator alloc)
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

        if (!transport_ && !streaming_transport_)
            return Unexpected(make_error(Error::NotReady, NOTE_ERR("no transport configured")));

        // Full streaming path: SAX-parse response directly from transport.
        // Requires both streaming transport and an allocator (for string interning).
        // Also requires Rsp to have a Sink type (generated responses do; ad-hoc ones don't).
        if constexpr (std::is_void_v<Rsp> || detail::has_sink<Rsp>::value) {
            if (streaming_transport_ && alloc_.has_value()) {
                // Type-erase the build lambda for the non-template core.
                auto build = [&](JsonBuilder& b) {
                    b.add("req", RequestT::notecard_request);
                    req.build(b);
                };
                BuildFn build_fn = [](JsonBuilder& b, void* p) {
                    (*static_cast<decltype(build)*>(p))(b);
                };

                StringPool pool(*alloc_);

                if constexpr (std::is_void_v<Rsp>) {
                    JsonSink null_sink;
                    auto ei = execute_streaming(*streaming_transport_, default_timeout_ms_, build_fn, &build, null_sink, pool);
                    if (ei.code != Error{}) return ApiResult<void>(ei);
                    return ApiResult<void>{};
                } else {
                    Rsp rsp_val{};
                    typename Rsp::Sink response_sink(rsp_val, pool);
                    auto ei = execute_streaming(*streaming_transport_, default_timeout_ms_, build_fn, &build, response_sink, pool);
                    if (ei.code != Error{}) return ApiResult<Rsp>(ei);
                    return ApiResult<Rsp>(std::move(rsp_val));
                }
            }
        }

        // Buffered fallback: requires a JsonBackend for build/parse.
        // Guarded by NOTE_NO_STD_STRING — when defined, the buffered path
        // (which needs std::string for response buffering) is unavailable.
#ifndef NOTE_NO_STD_STRING
        if (backend_) {
            return execute_buffered(req);
        }
#endif
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
#ifndef NOTE_NO_STD_STRING
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
#endif
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

#ifndef NOTE_NO_STD_STRING
    // Ad-hoc request with a builder callback.
    // Returns a unique_ptr<JsonReader> for backward compatibility.
    // The reader's lifetime is independent — it owns its data.
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

#endif // NOTE_NO_STD_STRING

    // Fire-and-forget typed command (generated request types).
    template<typename RequestT>
    Result<void> command_typed(const RequestT& req) {
        if (streaming_transport_) {
            auto build = [&](JsonBuilder& b) {
                b.add("cmd", RequestT::notecard_request);
                req.build(b);
            };
            BuildFn fn = [](JsonBuilder& b, void* p) {
                (*static_cast<decltype(build)*>(p))(b);
            };
            return streaming_transport_->send(fn, &build);
        }
        if (!transport_) return make_error(Error::NotReady, NOTE_ERR("no transport configured"));

        auto& builder = backend_->get_builder();
        builder.add("cmd", RequestT::notecard_request);
        req.build(builder);
        return transport_->send(builder.to_view());
    }

#ifndef NOTE_NO_STD_STRING
    // Fire-and-forget command.
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
#endif // NOTE_NO_STD_STRING

    void set_default_timeout(uint32_t ms) { default_timeout_ms_ = ms; }
    uint32_t default_timeout() const { return default_timeout_ms_; }

    /// Access the underlying transport.
    IBufferedTransport& transport() { return *transport_; }

    JsonBackend& backend() { return *backend_; }

private:
#ifndef NOTE_NO_STD_STRING
    template<typename RequestT>
    ApiResult<typename RequestT::Response> do_binary_send(RequestT& req) {
        auto src = req.binary_src_;

        // Pre-flight: check space and auto-reset if offset==0.
        if (req.binary_verify_) {
            // Reset on first segment (offset not set or == 0)
            if (!req.offset || *req.offset == 0) {
                auto clear = request("card.binary", [](JsonBuilder& b) {
                    b.add("delete", true);
                });
                if (!clear) {
                    return ApiResult<typename RequestT::Response>(
                        ErrorInfo{Error::SendFailed, Cause::Unspecified, NOTE_ERR("binary reset failed")});
                }
            }
            auto status = request("card.binary");
            if (!status) {
                return ApiResult<typename RequestT::Response>(
                    ErrorInfo{Error::SendFailed, Cause::Unspecified, NOTE_ERR("binary status query failed")});
            }
            auto max_bytes = (*status)->get_int("max", 0);
            if (max_bytes <= 0) {
                return ApiResult<typename RequestT::Response>(
                    ErrorInfo{Error::Overflow, Cause::Unspecified, NOTE_ERR("binary store not available")});
            }
            if (static_cast<int32_t>(src.size()) > max_bytes) {
                return ApiResult<typename RequestT::Response>(
                    ErrorInfo{Error::Overflow, Cause::Unspecified, NOTE_ERR("data exceeds binary store capacity")});
            }
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
            if (tx_ok) tx_ok = !!transport_->write(block, n);
        });
        if (tx_ok) {
            uint8_t eop = cobs_eop;
            tx_ok = !!transport_->write(&eop, 1);
        }
        if (!tx_ok) {
            transport_->reset();
            return ApiResult<typename RequestT::Response>(
                ErrorInfo{Error::SendFailed, Cause::HalError, NOTE_ERR("binary transmit failed")});
        }

        // Post-transmit verification: query card.binary status and confirm
        // the Notecard's stored MD5 matches what we sent.
        if (req.binary_verify_ && !md5_hex.empty()) {
            auto status = request("card.binary");
            if (!status) {
                return ApiResult<typename RequestT::Response>(
                    ErrorInfo{Error::ResponseLost, Cause::Unspecified, NOTE_ERR("binary verify query failed")});
            }
            auto stored_md5 = (*status)->get_string("status");
            if (!stored_md5.empty() && stored_md5 != string_view(md5_hex)) {
                return ApiResult<typename RequestT::Response>(
                    ErrorInfo{Error::ResponseLost, Cause::CrcMismatch, NOTE_ERR("binary verify: MD5 mismatch")});
            }
        }

        return result;
    }

    template<typename RequestT>
    ApiResult<typename RequestT::Response> do_binary_receive(RequestT& req) {
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
            auto r = transport_->read(chunk, sizeof(chunk), default_timeout_ms_);
            if (!r) {
                transport_->reset();
                return ApiResult<typename RequestT::Response>(
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
                transport_->reset();
                return ApiResult<typename RequestT::Response>(
                    ErrorInfo{Error::ResponseLost, Cause::CrcMismatch, "MD5 mismatch"});
            }
        }

        return result;
    }

#endif // NOTE_NO_STD_STRING

    /// Buffered execute: build JSON via backend, transact, parse response.
    /// Separated from execute() so LTO can eliminate it when backend_ is null.
    template<typename RequestT>
    ApiResult<typename RequestT::Response> execute_buffered(const RequestT& req) {
        using Rsp = typename RequestT::Response;

        Result<string_view> rsp = make_error(Error::NotReady, "");
        {
            auto& builder = backend_->get_builder();
            builder.add("req", RequestT::notecard_request);
            req.build(builder);
            rsp = transport_->transact(builder.to_view(), default_timeout_ms_);
        }
        if (!rsp) return Unexpected(rsp.error());

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

    /// Streaming execute core — template on transport to support both
    /// virtual (IStreamingTransport*) and concrete (StaticNotecard) dispatch.
    /// When called with a pointer, the compiler generates one shared copy.
    /// When called with a concrete ref, it devirtualizes on modern GCC.
    template<typename Transport>
    static ErrorInfo execute_streaming(Transport& t, uint32_t timeout_ms,
                                       BuildFn build_fn, void* ctx,
                                       JsonSink& inner_sink, StringPool& pool) {
        ErrorCaptureSink err_sink(inner_sink);
        auto rv = t.transact(build_fn, ctx, err_sink, timeout_ms);
        if (!rv) return rv.error();
        auto err = err_sink.captured_error();
        if (!err.empty())
            return ErrorInfo{Error::Notecard, Cause::Unspecified, pool.intern(err)};
        return {};
    }

    JsonBackend* backend_ = nullptr;
    IBufferedTransport* transport_ = nullptr;
    IStreamingTransport* streaming_transport_ = nullptr;
    uint32_t default_timeout_ms_ = 10000;
    std::optional<Allocator> alloc_;
    byte_span cobs_buf_{};          // optional external COBS working buffer
#ifndef NOTE_NO_MD5
    PlatformMd5 platform_md5_{};    // default MD5 implementation
    Md5Provider* md5_ = &platform_md5_;
#else
    Md5Provider* md5_ = nullptr;
#endif
};

} // namespace note
