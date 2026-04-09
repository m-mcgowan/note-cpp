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
        [[maybe_unused]] constexpr Safety safety = RequestT::safety;
#ifndef NOTE_NO_REQUEST_IDS
        const uint32_t req_id = request_ids_enabled_ ? next_request_id_++ : 0;
#else
        constexpr uint32_t req_id = 0;
#endif

        auto fields = [&](JsonBuilder& b) {
            if (req_id) b.add("id", static_cast<int32_t>(req_id));
            req.build(b);
        };
        BuildFn fields_fn = [](JsonBuilder& b, void* p) {
            (*static_cast<decltype(fields)*>(p))(b);
        };

        // All paths below go through transact_dispatch, which is non-template.
        // The void and generic paths are already fully type-erased. The custom
        // sink path does type-dependent setup (Sink, body handler) but the
        // actual transport call is type-erased. This means retry only needs to
        // re-invoke transact_dispatch — no per-type retry instantiation.

        if constexpr (std::is_void_v<Rsp>) {
            detail::NcErrorCapture nc_err;
            auto rv = execute_void(RequestT::notecard_request, fields_fn, &fields, nc_err, safety);
            if (!rv) return Unexpected(rv.error());
            if (!nc_err.empty()) {
                StringPool pool(alloc_);
                return ApiResult<void>(ErrorInfo{Error::Notecard, Cause::Unspecified, pool.intern(nc_err.view())});
            }
            return ApiResult<void>{};
        } else if constexpr (NOTE_PRINTABLE == 0
                             && detail::has_field_descs<RequestT>::value) {
            // Table-driven path: shared execute_generic (one copy for all types).
            // Body-having endpoints set up a body handler via req.body_handler_factory_;
            // GenericResponseSink forwards body events to it.
            constexpr bool has_body = detail::has_body_factory<RequestT>::value;
            alignas(body_sink_storage_align) char body_storage[has_body ? body_sink_storage_size : 1];
            BodyHandler body_handler{};
            if constexpr (has_body) {
                if (req.body_handler_factory_) {
                    StringPool pool(alloc_);
                    body_handler = req.body_handler_factory_(req.body_ptr_, pool, body_storage);
                }
            }
            Rsp rsp_val{};
            bool arena_exhausted = false;
            detail::NcErrorCapture nc_err;
            auto rv = execute_generic_retried(RequestT::notecard_request, fields_fn, &fields,
                                              &rsp_val, RequestT::field_descs_ptr(), RequestT::field_count,
                                              nc_err, arena_exhausted, safety, body_handler);
            if (!rv) return Unexpected(rv.error());
            if (!nc_err.empty()) {
                StringPool pool(alloc_);
                return ApiResult<Rsp>(ErrorInfo{Error::Notecard, Cause::Unspecified, pool.intern(nc_err.view())});
            }
            if (arena_exhausted)
                return ApiResult<Rsp>(ErrorInfo{Error::Overflow, Cause::Unspecified, NOTE_ERR("arena exhausted")});
            return ApiResult<Rsp>(std::move(rsp_val));
        } else {
            // Custom sink path: type-dependent setup, type-erased transport call.
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
            auto full_build = [&](JsonBuilder& b) {
                b.add("req", RequestT::notecard_request);
                fields_fn(b, &fields);
            };
            BuildFn full_fn = [](JsonBuilder& b, void* p) {
                (*static_cast<decltype(full_build)*>(p))(b);
            };
            auto dispatch = make_sax_dispatch(response_sink);
            detail::NcErrorCapture nc_err;
            auto rv = transact_retried(full_fn, &full_build, dispatch, nc_err, safety);
            if (!rv) return Unexpected(rv.error());
            if (!nc_err.empty())
                return ApiResult<Rsp>(ErrorInfo{Error::Notecard, Cause::Unspecified, pool.intern(nc_err.view())});
            return ApiResult<Rsp>(std::move(rsp_val));
        }
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
    /// req_type is the "req" field value (e.g. "card.temp").
    /// fields_fn serializes only the request-specific fields (not "req").
    Result<void> execute_generic(string_view req_type, BuildFn fields_fn, void* fields_ctx,
                                 void* rsp_storage, const FieldDesc* fields,
                                 uint8_t n_fields, detail::NcErrorCapture& nc_err,
                                 bool& arena_exhausted) {
        struct Ctx { string_view req; BuildFn fn; void* inner; };
        Ctx ctx{req_type, fields_fn, fields_ctx};
        BuildFn wrapped = [](JsonBuilder& b, void* p) {
            auto& c = *static_cast<Ctx*>(p);
            b.add("req", c.req);
            if (c.fn) c.fn(b, c.inner);
        };
        StringPool pool(alloc_);
        GenericResponseSink gsink{rsp_storage, fields, n_fields, &pool};
        auto dispatch = make_sax_dispatch(gsink);
        auto rv = stack_.transport.transact_dispatch(wrapped, &ctx, dispatch, default_timeout_ms_, nc_err);
        arena_exhausted = pool.exhausted();
        return rv;
    }

    /// Non-template execute for void-response endpoints.
    Result<void> execute_void(string_view req_type, BuildFn fields_fn, void* fields_ctx,
                              detail::NcErrorCapture& nc_err,
                              [[maybe_unused]] Safety safety = Safety::NonIdempotent) {
        struct Ctx { string_view req; BuildFn fn; void* inner; };
        Ctx ctx{req_type, fields_fn, fields_ctx};
        BuildFn wrapped = [](JsonBuilder& b, void* p) {
            auto& c = *static_cast<Ctx*>(p);
            b.add("req", c.req);
            if (c.fn) c.fn(b, c.inner);
        };
        NullSink null_sink;
        auto dispatch = make_sax_dispatch(null_sink);
        return transact_retried(wrapped, &ctx, dispatch, nc_err, safety);
    }

    /// Non-template execute_generic with body handler factory.
    /// Used by the unified singleton thunk — body endpoints pass their factory,
    /// non-body endpoints pass nullptr. Avoids per-type template instantiation.
    Result<void> execute_generic_with_body(
            string_view req_type, BuildFn fields_fn, void* fields_ctx,
            void* rsp_storage, const FieldDesc* rsp_fields, uint8_t n_fields,
            detail::NcErrorCapture& nc_err, bool& arena_exhausted,
            void* body_ptr, BodyHandlerFactory body_factory) {
        alignas(body_sink_storage_align) char body_storage[body_sink_storage_size];
        BodyHandler body_handler{};
        if (body_factory) {
            StringPool pool(alloc_);
            body_handler = body_factory(body_ptr, pool, body_storage);
        }
        return execute_generic_retried(req_type, fields_fn, fields_ctx,
                                        rsp_storage, rsp_fields, n_fields,
                                        nc_err, arena_exhausted, Safety::NonIdempotent, body_handler);
    }

    /// Access the transport stack (e.g. for binary I/O).
    Stack& stack() { return stack_; }

    /// Non-template transact with retry. Wraps transact_dispatch in retry_loop.
    Result<void> transact_retried(BuildFn build_fn, void* build_ctx,
                                  SaxDispatch dispatch,
                                  detail::NcErrorCapture& nc_err,
                                  [[maybe_unused]] Safety safety) {
#ifndef NOTE_NO_RETRY
        enforce_timing();
#endif
        auto rv = stack_.transport.transact_dispatch(build_fn, build_ctx, dispatch, default_timeout_ms_, nc_err);
#ifndef NOTE_NO_RETRY
        if (!rv && retry_policy_.max_retries > 0
            && detail::should_retry(rv.error().code, safety)) {
            struct Ctx {
                StaticNotecard* self;
                BuildFn fn; void* build_ctx;
                SaxDispatch dispatch;
                detail::NcErrorCapture* nc_err;
                Result<void>* rv;
            };
            Ctx ctx{this, build_fn, build_ctx, dispatch, &nc_err, &rv};
            auto ops = transport_ops();
            retry_loop(false, rv.error().code,
                [](void* c, Error* out) -> bool {
                    auto& x = *static_cast<Ctx*>(c);
                    x.nc_err->len = 0;
                    *x.rv = x.self->stack_.transport.transact_dispatch(
                        x.fn, x.build_ctx, x.dispatch, x.self->default_timeout_ms_, *x.nc_err);
                    if (*x.rv) return true;
                    *out = x.rv->error().code;
                    return false;
                },
                &ctx, ops, timing_, safety, retry_policy_);
        }
        record_timing();
#endif
        return rv;
    }

    /// Non-template execute_generic with retry.
    Result<void> execute_generic_retried(string_view req_type, BuildFn fields_fn, void* fields_ctx,
                                         void* rsp_storage, const FieldDesc* rsp_fields,
                                         uint8_t n_fields, detail::NcErrorCapture& nc_err,
                                         bool& arena_exhausted,
                                         [[maybe_unused]] Safety safety,
                                         BodyHandler body_handler = {}) {
        struct Ctx { string_view req; BuildFn fn; void* inner; };
        Ctx ctx{req_type, fields_fn, fields_ctx};
        BuildFn wrapped = [](JsonBuilder& b, void* p) {
            auto& c = *static_cast<Ctx*>(p);
            b.add("req", c.req);
            if (c.fn) c.fn(b, c.inner);
        };
        StringPool pool(alloc_);
        GenericResponseSink gsink{rsp_storage, rsp_fields, n_fields, &pool};
        if (body_handler) gsink.set_body_handler(body_handler);
        auto dispatch = make_sax_dispatch(gsink);
        auto rv = transact_retried(wrapped, &ctx, dispatch, nc_err, safety);
        arena_exhausted = pool.exhausted();
        return rv;
    }

private:
#ifndef NOTE_NO_RETRY
    RetryTransportOps transport_ops() {
        return {
            &stack_,
            [](void* c) -> uint32_t { return static_cast<Stack*>(c)->transport.millis(); },
            [](void* c, uint32_t ms) { static_cast<Stack*>(c)->transport.delay(ms); },
            [](void* c) { static_cast<Stack*>(c)->transport.reset(); },
        };
    }

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
