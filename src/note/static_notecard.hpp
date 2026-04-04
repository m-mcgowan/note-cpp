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
#include "json.hpp"
#include "notecard.hpp"
#include "retry.hpp"
#include "retry_policy.hpp"
#include "string_pool.hpp"
#include "streaming_transport.hpp"

#include <optional>
#include <type_traits>

namespace note {

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
            StringPool pool(alloc_);

            if constexpr (std::is_void_v<Rsp>) {
                struct NullSink {
                    void on_null(string_view) {}
                    void on_bool(string_view, bool) {}
                    void on_number(string_view, string_view) {}
                    void on_string(string_view, string_view) {}
                    void on_object_begin(string_view) {}
                    void on_object_end(string_view) {}
                    void on_array_begin(string_view) {}
                    void on_array_end(string_view) {}
                    void reset() {}
                } null_sink;
                ErrorCaptureSinkT<NullSink> err_sink(null_sink);
                auto rv = stack_.transport.transact(build_fn, &build, err_sink, default_timeout_ms_);
                if (!rv) return Unexpected(rv.error());
                auto err = err_sink.captured_error();
                if (!err.empty())
                    return ApiResult<void>(ErrorInfo{Error::Notecard, Cause::Unspecified, pool.intern(err)});
                return ApiResult<void>{};
            } else {
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
                ErrorCaptureSinkT<typename Rsp::Sink> err_sink(response_sink);
                auto rv = stack_.transport.transact(build_fn, &build, err_sink, default_timeout_ms_);
                if (!rv) return Unexpected(rv.error());
                auto err = err_sink.captured_error();
                if (!err.empty())
                    return ApiResult<Rsp>(ErrorInfo{Error::Notecard, Cause::Unspecified, pool.intern(err)});
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
