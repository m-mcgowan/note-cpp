#pragma once

/// @file static_notecard.hpp
/// StaticNotecard<Stack> — zero-vtable Notecard for constrained platforms.
///
/// Owns the transport stack by value. All calls go through concrete types —
/// no virtual dispatch, no vtable entries in .data. Uses the same
/// execute_streaming() core as Notecard, just with a concrete transport ref
/// instead of an IStreamingTransport*.
///
/// Usage:
///   using SerialNotecard = note::StaticNotecard<note::arduino::SerialTransportStack<>>;
///   SerialNotecard nc(Serial, 9600, arena_allocator(arena));
///   note::Api api(nc);
///   api.hub.set().product("com.example").execute();

#include "allocator.hpp"
#include "compiler.hpp"
#include "generic_sink.hpp"
#include "json.hpp"
#include "notecard.hpp"
#include "retry.hpp"
#include "retry_policy.hpp"
#include "string_pool.hpp"
#include "streaming_transport.hpp"

#include <optional>
#include <type_traits>

namespace note {

namespace detail {
    /// Detects request types that provide a generic field descriptor table.
    template<typename T, typename = void>
    struct has_field_descs : std::false_type {};
    template<typename T>
    struct has_field_descs<T, std::void_t<decltype(T::field_descs_ptr())>> : std::true_type {};
}

/// Notecard implementation with zero virtual dispatch overhead.
/// Stack must provide a `transport` member with `transact(BuildFn, void*, JsonSink&, uint32_t)`.
template<typename Stack>
class StaticNotecard {
public:
    /// Construct by forwarding args to the Stack constructor.
    template<typename... Args>
    explicit StaticNotecard(Allocator alloc, Args&&... args)
        : stack_(std::forward<Args>(args)...)
        , alloc_(alloc) {}

    void set_allocator(Allocator alloc) { alloc_ = alloc; }
    void set_default_timeout(uint32_t ms) { default_timeout_ms_ = ms; }
    uint32_t default_timeout() const { return default_timeout_ms_; }

#ifndef NOTE_NO_RETRY
    void set_retry_policy(RetryPolicy policy) { retry_policy_ = policy; }
    void set_inter_transaction_gap(uint32_t ms) { timing_.min_gap_ms = ms; }
#endif
#ifndef NOTE_NO_REQUEST_IDS
    void set_request_ids(bool enabled) { request_ids_enabled_ = enabled; }
#endif

    template<typename RequestT>
    ApiResult<typename RequestT::Response> execute(const RequestT& req) {
        using Rsp = typename RequestT::Response;
        constexpr Safety safety = RequestT::safety;
#ifndef NOTE_NO_REQUEST_IDS
        const uint32_t req_id = request_ids_enabled_ ? next_request_id_++ : 0;
#else
        constexpr uint32_t req_id = 0;
#endif

        auto build = [&](JsonBuilder& b) {
            b.add("req", RequestT::notecard_request);
            if (req_id) b.add("id", static_cast<int32_t>(req_id));
            req.build(b);
        };
        BuildFn build_fn = [](JsonBuilder& b, void* p) {
            (*static_cast<decltype(build)*>(p))(b);
        };

        auto attempt = [&]() -> ApiResult<Rsp> {
            detail::NcErrorCapture nc_err;

            if constexpr (std::is_void_v<Rsp>) {
                auto rv = execute_void(build_fn, &build, nc_err);
                if (!rv) return Unexpected(rv.error());
                if (!nc_err.empty()) {
                    StringPool pool(alloc_);
                    return ApiResult<void>(ErrorInfo{Error::Notecard, Cause::Unspecified, pool.intern(nc_err.view())});
                }
                return ApiResult<void>{};
            } else if constexpr (NOTE_PRINTABLE == 0
                                 && detail::has_field_descs<RequestT>::value
                                 && (!detail::has_body_factory<RequestT>::value || !NOTE_RESPONSE_BODY)) {
                // Table-driven path: shared execute_generic (one copy for all types).
                Rsp rsp_val{};
                bool arena_exhausted = false;
                auto rv = execute_generic(build_fn, &build, &rsp_val,
                                          RequestT::field_descs_ptr(), RequestT::field_count,
                                          nc_err, arena_exhausted);
                if (!rv) return Unexpected(rv.error());
                if (!nc_err.empty()) {
                    StringPool pool(alloc_);
                    return ApiResult<Rsp>(ErrorInfo{Error::Notecard, Cause::Unspecified, pool.intern(nc_err.view())});
                }
                if (arena_exhausted)
                    return ApiResult<Rsp>(ErrorInfo{Error::Overflow, Cause::Unspecified, NOTE_ERR("arena exhausted")});
                return ApiResult<Rsp>(std::move(rsp_val));
            } else {
                // Custom sink path: for body-enabled endpoints or types without field_descs.
                StringPool pool(alloc_);
                Rsp rsp_val{};
                typename Rsp::Sink response_sink(rsp_val, pool);
                alignas(body_sink_storage_align) char body_storage[body_sink_storage_size];
                if constexpr (detail::has_body_factory<RequestT>::value
                              && detail::has_set_body_handler<typename Rsp::Sink>::value) {
                    if (req.body_handler_factory_) {
                        auto bh = req.body_handler_factory_(req.body_ptr_, pool, body_storage);
                        if (bh) response_sink.set_body_handler(bh);
                    }
                }
                auto dispatch = make_sax_dispatch(response_sink);
                auto rv = stack_.transport.transact_dispatch(build_fn, &build, dispatch, default_timeout_ms_, nc_err);
                if (!rv) return Unexpected(rv.error());
                if (!nc_err.empty())
                    return ApiResult<Rsp>(ErrorInfo{Error::Notecard, Cause::Unspecified, pool.intern(nc_err.view())});
                return ApiResult<Rsp>(std::move(rsp_val));
            }
        };

#ifndef NOTE_NO_RETRY
        auto reset = [&]() { stack_.transport.reset(); };

        return retry_transaction<ApiResult<Rsp>>(
            stack_.transport, timing_, safety, retry_policy_,
            attempt, reset);
#else
        return attempt();
#endif
    }

    template<typename RequestT>
    Result<void> command_typed(const RequestT& req) {
#ifndef NOTE_NO_RETRY
        enforce_timing();
#endif
        auto build = [&](JsonBuilder& b) {
            b.add("cmd", RequestT::notecard_request);
            req.build(b);
        };
        BuildFn build_fn = [](JsonBuilder& b, void* p) {
            (*static_cast<decltype(build)*>(p))(b);
        };
        auto result = stack_.transport.send(build_fn, &build);
#ifndef NOTE_NO_RETRY
        record_timing();
#endif
        return result;
    }

    /// Type-erased send (fire-and-forget). Used by generated command() methods
    /// via send_fn_ — a single shared function pointer for all request types.
    Result<void> send_command(BuildFn build_fn, void* ctx) {
#ifndef NOTE_NO_RETRY
        enforce_timing();
#endif
        auto result = stack_.transport.send(build_fn, ctx);
#ifndef NOTE_NO_RETRY
        record_timing();
#endif
        return result;
    }

    /// Non-template execute for GenericResponseSink endpoints.
    /// Shared by all endpoints that use table-driven field dispatch.
    Result<void> execute_generic(BuildFn build_fn, void* build_ctx,
                                 void* rsp_storage, const FieldDesc* fields,
                                 uint8_t n_fields, detail::NcErrorCapture& nc_err,
                                 bool& arena_exhausted) {
        StringPool pool(alloc_);
        GenericResponseSink gsink{rsp_storage, fields, n_fields, &pool};
        auto dispatch = make_sax_dispatch(gsink);
        auto rv = stack_.transport.transact_dispatch(build_fn, build_ctx, dispatch, default_timeout_ms_, nc_err);
        arena_exhausted = pool.exhausted();
        return rv;
    }

    /// Non-template execute for void-response endpoints.
    Result<void> execute_void(BuildFn build_fn, void* build_ctx,
                              detail::NcErrorCapture& nc_err) {
        NullSink null_sink;
        auto dispatch = make_sax_dispatch(null_sink);
        return stack_.transport.transact_dispatch(build_fn, build_ctx, dispatch, default_timeout_ms_, nc_err);
    }

    /// Access the transport stack (e.g. for binary I/O).
    Stack& stack() { return stack_; }

private:
#ifndef NOTE_NO_RETRY
    void enforce_timing() {
        if (!timing_.has_previous) return;
        uint32_t elapsed = stack_.transport.millis() - timing_.last_transaction_end_ms;
        if (elapsed < timing_.min_gap_ms)
            stack_.transport.delay(timing_.min_gap_ms - elapsed);
    }

    void record_timing() {
        timing_.last_transaction_end_ms = stack_.transport.millis();
        timing_.has_previous = true;
    }
#endif

    Stack stack_;
    Allocator alloc_;
    uint32_t default_timeout_ms_ = 10000;
#ifndef NOTE_NO_RETRY
    RetryPolicy retry_policy_{};
    TransactionTiming timing_{};
#endif
#ifndef NOTE_NO_REQUEST_IDS
    uint32_t next_request_id_ = 1;
    bool request_ids_enabled_ = true;
#endif
};

} // namespace note
