#pragma once

#include "allocator.hpp"
#include "binary_request.hpp"
#include "json.hpp"
#include "md5.hpp"
#include "safety.hpp"
#include "span.hpp"
#include "string_pool.hpp"
#include "transport.hpp"
#include "transport/cobs.hpp"

#include <optional>
#include <string>
#include <type_traits>

namespace note {

namespace detail {
    template<typename T, typename = void>
    struct has_intern_strings : std::false_type {};
    template<typename T>
    struct has_intern_strings<T, std::void_t<decltype(std::declval<T>().intern_strings(std::declval<StringPool&>()))>> : std::true_type {};
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

    Notecard(JsonBackend& backend, ITransport& transport)
        : backend_(&backend)
        , transport_(&transport)
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

        if (!transport_) return Unexpected(make_error(Error::NotReady, "no transport configured"));

        auto& builder = backend_->get_builder();
        builder.add("req", RequestT::notecard_request);
        req.build(builder);

        auto rsp = transport_->transact(builder.to_view(), default_timeout_ms_);
        if (!rsp) return Unexpected(rsp.error());

        auto& reader = backend_->get_reader(*rsp);
        if (reader.has_error()) {
            return make_error(Error::Json, "JSON parse error");
        }
        auto err = reader.get_error();
        if (!err.empty()) {
            if (alloc_.has_value()) {
                // Intern error message into allocator-backed storage
                StringPool pool(*alloc_);
                ErrorInfo ei{Error::Notecard, Cause::Unspecified, pool.intern(err)};
                return ApiResult<Rsp>(std::move(ei));
            }
            // Fallback: use parse_response() to get an owned reader that
            // keeps the error message string_view alive in ApiResult.
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

    // Ad-hoc request with a builder callback.
    // Returns a unique_ptr<JsonReader> for backward compatibility.
    // The reader's lifetime is independent — it owns its data.
    Result<std::unique_ptr<JsonReader>> request(
            string_view req_type,
            std::function<void(JsonBuilder&)> build_fn = {}) {
        if (!transport_) return make_error(Error::NotReady, "no transport configured");

        auto& builder = backend_->get_builder();
        builder.add("req", req_type);
        if (build_fn) build_fn(builder);

        auto rsp = transport_->transact(builder.to_view(), default_timeout_ms_);
        if (!rsp) return Unexpected(rsp.error());

        auto reader = backend_->parse_response(*rsp);
        if (reader->has_error()) {
            return make_error(Error::Json, reader->get_error());
        }
        // Note: we don't check get_error() here — the caller receives the
        // reader directly and can inspect {"err":"..."} themselves.
        return reader;
    }

    // Fire-and-forget typed command (generated request types).
    template<typename RequestT>
    Result<void> command_typed(const RequestT& req) {
        if (!transport_) return make_error(Error::NotReady, "no transport configured");

        auto& builder = backend_->get_builder();
        builder.add("cmd", RequestT::notecard_request);
        req.build(builder);
        return transport_->send(builder.to_view());
    }

    // Fire-and-forget command.
    Result<void> command(string_view cmd_type,
                         std::function<void(JsonBuilder&)> build_fn = {}) {
        if (!transport_) return make_error(Error::NotReady, "no transport configured");

        auto& builder = backend_->get_builder();
        builder.add("cmd", cmd_type);
        if (build_fn) build_fn(builder);
        return transport_->send(builder.to_view());
    }

    void set_default_timeout(uint32_t ms) { default_timeout_ms_ = ms; }
    uint32_t default_timeout() const { return default_timeout_ms_; }

    /// Access the underlying transport.
    ITransport& transport() { return *transport_; }

    JsonBackend& backend() { return *backend_; }

private:
    template<typename RequestT>
    ApiResult<typename RequestT::Response> do_binary_send(RequestT& req) {
        auto src = req.binary_src_;
        req.cobs = static_cast<int32_t>(cobs_encoded_length(src.data(), src.size()));
        if (md5_) req.status = md5_->compute(src.data(), src.size());

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
                ErrorInfo{Error::SendFailed, Cause::HalError, "binary transmit failed"});
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
                    ErrorInfo{Error::ResponseLost, Cause::Timeout, "binary receive timeout"});
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
            if (actual != std::string(expected_md5.data(), expected_md5.size())) {
                transport_->reset();
                return ApiResult<typename RequestT::Response>(
                    ErrorInfo{Error::ResponseLost, Cause::CrcMismatch, "MD5 mismatch"});
            }
        }

        return result;
    }

    JsonBackend* backend_ = nullptr;
    ITransport* transport_ = nullptr;
    uint32_t default_timeout_ms_ = 10000;
    std::optional<Allocator> alloc_;
    byte_span cobs_buf_{};          // optional external COBS working buffer
    PlatformMd5 platform_md5_{};    // default MD5 implementation
    Md5Provider* md5_ = &platform_md5_;
};

} // namespace note
