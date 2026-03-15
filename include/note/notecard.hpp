#pragma once

#include "allocator.hpp"
#include "json.hpp"
#include "safety.hpp"
#include "string_pool.hpp"

#include <functional>
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
    // Transport callables:
    //   RequestFn: send a JSON request, receive response as string_view into
    //              transport's member buffer. View valid until next call.
    //   SendFn:    send a JSON command string (fire-and-forget, no response).
    using RequestFn = std::function<Result<string_view>(string_view request, uint32_t timeout_ms)>;
    using SendFn    = std::function<Result<void>(string_view request)>;

    Notecard(JsonBackend& backend, RequestFn request_fn, SendFn send_fn = {})
        : backend_(backend)
        , request_fn_(std::move(request_fn))
        , send_fn_(std::move(send_fn))
    {
        // If no send function provided, derive one from request (discard response).
        if (!send_fn_) {
            send_fn_ = [this](string_view req) -> Result<void> {
                auto r = request_fn_(req, default_timeout_ms_);
                if (!r) return Unexpected(r.error());
                return {};
            };
        }
    }

    // Configure an allocator for response string interning.
    // When set, execute() copies response string_view fields into the
    // allocator's backing store (e.g. a MonotonicArena) so they survive
    // transport buffer reuse. The parsing strategy is still dictated by
    // the backend (tree-parse or SAX).
    void set_allocator(Allocator alloc) { alloc_ = alloc; }
    void clear_allocator() { alloc_.reset(); }

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

        auto& builder = backend_.get_builder();
        builder.add("req", RequestT::notecard_request);
        req.build(builder);

        auto rsp = request_fn_(builder.to_view(), default_timeout_ms_);
        if (!rsp) return Unexpected(rsp.error());

        auto& reader = backend_.get_reader(*rsp);
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
            auto owned = backend_.parse_response(*rsp);
            ErrorInfo ei{Error::Notecard, Cause::Unspecified, owned->get_error()};
            return ApiResult<Rsp>(std::move(ei), std::move(owned));
        }
        if constexpr (std::is_void_v<Rsp>) {
            return ApiResult<void>{};
        } else {
            auto result = Rsp::parse(reader);
            if constexpr (detail::has_intern_strings<Rsp>::value) {
                if (alloc_.has_value()) {
                    StringPool pool(*alloc_);
                    result.intern_strings(pool);
                }
            }
            return result;
        }
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
        auto& builder = backend_.get_builder();
        builder.add("req", req_type);
        if (build_fn) build_fn(builder);

        auto rsp = request_fn_(builder.to_view(), default_timeout_ms_);
        if (!rsp) return Unexpected(rsp.error());

        auto reader = backend_.parse_response(*rsp);
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
        auto& builder = backend_.get_builder();
        builder.add("cmd", RequestT::notecard_request);
        req.build(builder);
        return send_fn_(builder.to_view());
    }

    // Fire-and-forget command.
    Result<void> command(string_view cmd_type,
                         std::function<void(JsonBuilder&)> build_fn = {}) {
        auto& builder = backend_.get_builder();
        builder.add("cmd", cmd_type);
        if (build_fn) build_fn(builder);
        return send_fn_(builder.to_view());
    }

    void set_default_timeout(uint32_t ms) { default_timeout_ms_ = ms; }
    uint32_t default_timeout() const { return default_timeout_ms_; }

    // Raw transport access — used by note-cpp-app channel variants.
    // Returns string_view into transport's member buffer (valid until next call).
    Result<string_view> transact(string_view json, uint32_t timeout_ms) {
        return request_fn_(json, timeout_ms);
    }
    Result<void> send(string_view json) {
        return send_fn_(json);
    }

    JsonBackend& backend() { return backend_; }

private:
    JsonBackend& backend_;
    RequestFn request_fn_;
    SendFn send_fn_;
    uint32_t default_timeout_ms_ = 10000;
    std::optional<Allocator> alloc_;
};

} // namespace note
