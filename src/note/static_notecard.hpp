#pragma once

/// @file static_notecard.hpp
/// StaticNotecard<Stack> — zero-vtable Notecard for constrained platforms.
///
/// Owns the transport stack by value. All calls go through concrete types —
/// no virtual dispatch, no vtable entries in .data. Uses the same
/// execute_streaming() core as Notecard, just with a concrete transport ref
/// instead of an ITransact*.
///
/// Usage:
///   using SerialNotecard = note::StaticNotecard<note::arduino::SerialTransportStack<>>;
///   SerialNotecard nc(Serial, 9600, arena_allocator(arena));
///   note::Api api(nc);
///   api.hub.set().product("com.example").execute();

#include "allocator.hpp"
#include "bus_lock.hpp"
#include "compiler.hpp"
#include "generic_sink.hpp"
#include "json.hpp"
#include "json_render.hpp"
#include "notecard.hpp"
#include "retry.hpp"
#include "retry_policy.hpp"
#include "string_pool.hpp"
#include "protocol.hpp"

#include <optional>
#include <type_traits>

namespace note {

namespace detail {

/// Shared "req"-prefixed BuildFn wrapper. Both `execute_void` and
/// `execute_generic_retried` need a BuildFn that emits `"req":"..."`
/// then delegates to the per-endpoint field builder. Extracting them to
/// a single non-template helper avoids a ~130 B duplication per
/// StaticNotecard instantiation per call site.
struct ReqWrapCtx {
    string_view req;
    BuildFn fn;
    void* inner;
};
inline void req_wrap_build(JsonBuilder& b, void* p) {
    auto& c = *static_cast<ReqWrapCtx*>(p);
    add_flash(b, flash(common_keys::req), c.req);
    if (c.fn) c.fn(b, c.inner);
}

/// RAII operation-scope lock guard for StaticNotecard entry points.
///
/// Primary template: stores a pointer to the Lock, acquires on construction,
/// releases on destruction. For a real Lockable (std::recursive_mutex etc.),
/// this gives correct RAII release even on thrown exceptions.
///
/// The NullLock specialization below has NO members and trivial ctor/dtor,
/// so GCC (including 7.3 for AVR) eliminates it entirely when Lock=NullLock.
/// Contrast with a pointer-carrying struct where GCC 7.3 may keep the
/// stack slot even when the pointer is never read.
template<typename Lock>
struct StaticNcOpGuard {
    Lock* lock_;
    explicit StaticNcOpGuard(Lock* l) : lock_(l) { l->lock(); }
    ~StaticNcOpGuard() { lock_->unlock(); }
    StaticNcOpGuard(const StaticNcOpGuard&) = delete;
    StaticNcOpGuard& operator=(const StaticNcOpGuard&) = delete;
};

/// NullLock specialization: zero members, trivial ctor/dtor.
/// GCC eliminates this entirely — zero flash, zero RAM, zero instructions.
template<>
struct StaticNcOpGuard<NullLock> {
    explicit StaticNcOpGuard(NullLock*) noexcept {}
    ~StaticNcOpGuard() noexcept {}
};

} // namespace detail

// Forward declaration so TxnOpGuard can reference StaticNotecard.
template<typename Stack, typename Lock> class StaticNotecard;

namespace detail {

/// Operation-scope RTX/CTX readiness guard for StaticNotecard.
///
/// When NOTE_TXN_HANDSHAKE is enabled, this guard tracks nesting depth via
/// `StaticNotecard::op_depth_` (private — friend access) and calls
/// `begin_operation()` on the outermost entry and `end_operation()` on the
/// outermost exit. Paired with the `OpGuard` (which handles locking) in
/// each public StaticNotecard entry point.
///
/// When NOTE_TXN_HANDSHAKE is disabled (the AVR default), the guard has NO
/// members and trivial ctor/dtor — GCC eliminates it entirely, just like
/// `StaticNcOpGuard<NullLock>`. Net cost on AVR: zero flash, zero RAM.
#if NOTE_TXN_HANDSHAKE
template<typename Stack, typename Lock>
struct StaticNcTxnOpGuard {
    StaticNotecard<Stack, Lock>& nc_;
    bool outermost_;
    explicit StaticNcTxnOpGuard(StaticNotecard<Stack, Lock>& nc)
        : nc_(nc), outermost_(nc.op_depth_++ == 0) {
        if (outermost_) nc_.stack_.transport.begin_operation(nc_.default_timeout_ms_);
    }
    ~StaticNcTxnOpGuard() {
        if (outermost_) nc_.stack_.transport.end_operation();
        --nc_.op_depth_;
    }
    StaticNcTxnOpGuard(const StaticNcTxnOpGuard&) = delete;
    StaticNcTxnOpGuard& operator=(const StaticNcTxnOpGuard&) = delete;
};
#else
template<typename Stack, typename Lock>
struct StaticNcTxnOpGuard {
    explicit StaticNcTxnOpGuard(StaticNotecard<Stack, Lock>&) noexcept {}
    ~StaticNcTxnOpGuard() noexcept {}
};
#endif

} // namespace detail

/// Notecard implementation with zero virtual dispatch overhead.
///
/// Stack must provide a `transport` member with `transact(BuildFn, void*, JsonSink&, uint32_t)`.
///
/// @tparam Stack   Transport stack type. Owns the HAL and framing layers.
/// @tparam Lock    Compile-time operation lock (default: NullLock — zero size and zero cost via
///                 empty-base optimization). Supply a recursive lock type here for multi-threaded
///                 or shared-bus use. The lock **must** be recursive: same-thread nested entry
///                 points re-acquire it on the same thread; a non-recursive lock would deadlock.
template<typename Stack, typename Lock = NullLock>
class StaticNotecard : private Lock {
public:
    /// Construct by forwarding args to the Stack constructor.
    template<typename... Args>
    explicit StaticNotecard(Allocator alloc, Args&&... args)
        : stack_(std::forward<Args>(args)...)
        , alloc_(alloc) { publish_singleton_allocator_(); }

    void set_allocator(Allocator alloc) { alloc_ = alloc; publish_singleton_allocator_(); }
    void set_default_timeout(uint32_t ms) { default_timeout_ms_ = ms; }
    uint32_t default_timeout() const { return default_timeout_ms_; }

#if !NOTE_NO_RETRY
    void set_retry_policy(RetryPolicy policy) { retry_policy_ = policy; }
    void set_inter_transaction_gap(uint32_t ms) { timing_.min_gap_ms = ms; }
#endif
#if !NOTE_NO_REQUEST_IDS
    void set_request_ids(bool enabled) { request_ids_enabled_ = enabled; }
#endif

    template<typename RequestT>
    ApiResult<typename RequestT::Response> execute(const RequestT& req) {
        OpGuard op_{static_cast<Lock*>(this)};
        TxnOpGuard txn_op_{*this};
        using Rsp = typename RequestT::Response;
        [[maybe_unused]] constexpr Safety safety = RequestT::safety;
#if !NOTE_NO_REQUEST_IDS
        const uint32_t req_id = request_ids_enabled_ ? next_request_id_++ : 0;
#else
        constexpr uint32_t req_id = 0;
#endif

        auto fields = [&](JsonBuilder& b) {
            if (req_id) add_flash(b, flash(detail::common_keys::id),
                                  static_cast<json_int_t>(req_id));
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
            // Route through the shared non-template execute_void() to prevent
            // per-RequestT code duplication. The OpGuard op_ acquired above is
            // held throughout; execute_void()'s own OpGuard is a recursive
            // re-acquire (no-op for NullLock, recursive for real locks).
            // TxnOpGuard tracks outermost depth so begin/end_operation fire once.
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
            using meta_ = ::note::detail::request_traits<RequestT>;
            auto rv = execute_generic_retried(RequestT::notecard_request, fields_fn, &fields,
                                              &rsp_val, meta_::field_descs_ptr(), meta_::field_count,
                                              nc_err, arena_exhausted, safety, body_handler);
            if (!rv) return Unexpected(rv.error());
            if (!nc_err.empty()) {
                StringPool pool(alloc_);
                return ApiResult<Rsp>(ErrorInfo{Error::Notecard, Cause::Unspecified, pool.intern(nc_err.view())});
            }
            if (arena_exhausted)
                return ApiResult<Rsp>(ErrorInfo{Error::Overflow, Cause::Unspecified, NOTE_ERR("arena exhausted")});
            detail::attach_allocator(rsp_val, alloc_);
            return ApiResult<Rsp>(std::move(rsp_val));
        } else if constexpr (NOTE_PRINTABLE) {
            // Custom sink path: per-type Sink when ResponseField implements
            // Printable (Arduino). The generic table-driven path above handles
            // all endpoints on non-Arduino targets.
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
                add_flash(b, flash(detail::common_keys::req), RequestT::notecard_request);
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
            detail::attach_allocator(rsp_val, alloc_);
            return ApiResult<Rsp>(std::move(rsp_val));
        } else {
            static_assert(detail::has_field_descs<RequestT>::value,
                "endpoint must have field descriptors for StaticNotecard");
            NOTE_UNREACHABLE();
        }
    }

    template<typename RequestT>
    Result<void> command_typed(const RequestT& req) {
        OpGuard op_{static_cast<Lock*>(this)};
        TxnOpGuard txn_op_{*this};
#if !NOTE_NO_RETRY
        enforce_timing();
#endif
        auto build = [&](JsonBuilder& b) {
            add_flash(b, flash(detail::common_keys::cmd), RequestT::notecard_request);
            req.build(b);
        };
        BuildFn build_fn = [](JsonBuilder& b, void* p) {
            (*static_cast<decltype(build)*>(p))(b);
        };
        BuildFnRequestSource src(build_fn, &build);
        auto result = stack_.transport.send(src.as_source());
#if !NOTE_NO_RETRY
        record_timing();
#endif
        return result;
    }

    /// Type-erased send (fire-and-forget). Used by generated command() methods
    /// via send_fn_ — a single shared function pointer for all request types.
    Result<void> send_command(BuildFn build_fn, void* ctx) {
        OpGuard op_{static_cast<Lock*>(this)};
        TxnOpGuard txn_op_{*this};
#if !NOTE_NO_RETRY
        enforce_timing();
#endif
        BuildFnRequestSource src(build_fn, ctx);
        auto result = stack_.transport.send(src.as_source());
#if !NOTE_NO_RETRY
        record_timing();
#endif
        return result;
    }

    /// Non-template execute for void-response endpoints.
    Result<void> execute_void(string_view req_type, BuildFn fields_fn, void* fields_ctx,
                              detail::NcErrorCapture& nc_err,
                              [[maybe_unused]] Safety safety = Safety::NonIdempotent) {
        OpGuard op_{static_cast<Lock*>(this)};
        TxnOpGuard txn_op_{*this};
        detail::ReqWrapCtx ctx{req_type, fields_fn, fields_ctx};
        NullSink null_sink;
        auto dispatch = make_sax_dispatch(null_sink);
        return transact_retried(&detail::req_wrap_build, &ctx, dispatch, nc_err, safety);
    }

    /// Non-template execute_generic with body handler factory.
    /// Used by the unified singleton thunk — body endpoints pass their factory,
    /// non-body endpoints pass nullptr. Avoids per-type template instantiation.
    Result<void> execute_generic_with_body(
            string_view req_type, BuildFn fields_fn, void* fields_ctx,
            void* rsp_storage, const FieldDesc* rsp_fields, uint8_t n_fields,
            detail::NcErrorCapture& nc_err, bool& arena_exhausted,
            void* body_ptr, BodyHandlerFactory body_factory,
            Safety safety = Safety::NonIdempotent) {
        OpGuard op_{static_cast<Lock*>(this)};
        TxnOpGuard txn_op_{*this};
        alignas(body_sink_storage_align) char body_storage[body_sink_storage_size];
        BodyHandler body_handler{};
        if (body_factory) {
            StringPool pool(alloc_);
            body_handler = body_factory(body_ptr, pool, body_storage);
        }
        return execute_generic_retried(req_type, fields_fn, fields_ctx,
                                       rsp_storage, rsp_fields, n_fields,
                                       nc_err, arena_exhausted, safety, body_handler);
    }

    /// Access the transport stack (e.g. for binary I/O).
    Stack& stack() { return stack_; }

    /// Send a pre-built JSON request and read raw response bytes back into
    /// `rsp`. The escape hatch under `note::Api` and the typed-execute path
    /// — use it when your firmware needs to skip the parser entirely (e.g.
    /// AVR-class targets). The response stays in `rsp`; wrap it in
    /// `note::JsonView(...)` to scan known fields, including the
    /// `Result<string_view>`-unwrapping ctor:
    ///
    ///     auto v = note::JsonView(nc.transact_raw(req, rsp));
    ///     float t = v.get_float("value");
    ///
    /// This is the non-template overload — pre-rendered `string_view` in,
    /// `Result<string_view>` of the response back. The two templates below
    /// forward to it for `JsonBuf`/`json<...>()` (anything with `.view()`)
    /// and for response buffers declared as `char rsp[N]` (deduces N).
    Result<string_view> transact_raw(string_view req, char* rsp, size_t n,
                                     uint32_t timeout_ms = 10000) {
        OpGuard op_{static_cast<Lock*>(this)};
        TxnOpGuard txn_op_{*this};
        return stack_.transport.transact_raw(req, rsp, n, timeout_ms);
    }

    /// Forwarder: any request type with `.view()` (e.g. `note::JsonBuf<N>`,
    /// `note::json<...>()` result). Calls the non-template overload above.
    template<typename T, typename = decltype(std::declval<const T&>().view())>
    Result<string_view> transact_raw(const T& req, char* rsp, size_t n,
                                     uint32_t timeout_ms = 10000) {
        return transact_raw(req.view(), rsp, n, timeout_ms);
    }

    /// Forwarder: response buffer declared as `char rsp[N]` — deduces `N`,
    /// no `sizeof(rsp)` needed. Forwards to whichever overload above
    /// matches `req`.
    template<typename T, size_t N>
    Result<string_view> transact_raw(const T& req, char (&rsp)[N],
                                     uint32_t timeout_ms = 10000) {
        return transact_raw(req, rsp, N, timeout_ms);
    }

    /// In-place variant: `buf` is used both to render the request *and* to
    /// receive the response (saving N bytes of RAM versus the separate
    /// request/response buffer pattern). The lambda receives a writer
    /// (`auto& w`) with the same `add()`/`begin_object()`/`close()` shape
    /// as `JsonBuf`. After it returns, the rendered bytes are sent over
    /// the transport, then the response overwrites `buf`. The returned
    /// `string_view` points into `buf`.
    ///
    ///     char buf[64];
    ///     auto v = note::JsonView(nc.transact_raw_inplace(buf, [&](auto& w) {
    ///         w.add("req", "card.temp");
    ///     }));
    ///     float t = v.get_float(K("value"));
    ///
    /// The shared buffer is safe because the underlying `transport.transact_raw`
    /// fully drains the request bytes before reading the response (the HAL
    /// `transmit()` returns only after flushing).
    template<size_t N, typename Fn>
    Result<string_view> transact_raw_inplace(char (&buf)[N], Fn&& build,
                                             uint32_t timeout_ms = 10000) {
        OpGuard op_{static_cast<Lock*>(this)};
        TxnOpGuard txn_op_{*this};
        // Type-erase the lambda via a stateless trampoline so the bulk of
        // the work (`transact_raw_inplace_impl_`) is one out-of-line copy
        // shared by every call site. The trampoline is per-Fn but tiny
        // (cast + call); the impl absorbs JsonRender ctor, close(),
        // overflow check, and the transport hop.
        InplaceBuilder trampoline = [](JsonRender& w, const void* ctx) {
            (*static_cast<const std::remove_reference_t<Fn>*>(ctx))(w);
        };
        return transact_raw_inplace_impl_(buf, N, trampoline,
                                          static_cast<const void*>(&build),
                                          timeout_ms);
    }

    // ── Operation-session guards ──────────────────────────────────────────
    //
    // Mirror of Notecard::exclusive() / Notecard::keep_ready().
    // See the Notecard counterparts for the full usage documentation.
    //
    // StaticNotecard differences:
    //   exclusive(): the lock is the EBO base class `Lock`. A `NullLock`
    //     base produces a zero-cost no-op guard (no lock/unlock calls and
    //     no stored pointer — the specialization has no members).
    //   keep_ready(): gated by NOTE_TXN_HANDSHAKE exactly like the
    //     polymorphic version; trivial empty guard when the flag is off.

    /// RAII guard returned by exclusive(). Holds the compile-time lock for
    /// its lifetime; non-copyable, non-movable. Zero-cost when Lock=NullLock
    /// (the NullLock specialization of StaticNcOpGuard has no members and
    /// trivial ctor/dtor, so the entire guard is eliminated by the compiler).
    struct ExclusiveSession {
        ExclusiveSession(const ExclusiveSession&)            = delete;
        ExclusiveSession& operator=(const ExclusiveSession&) = delete;
        ExclusiveSession(ExclusiveSession&&)                 = delete;
        ExclusiveSession& operator=(ExclusiveSession&&)      = delete;
        ~ExclusiveSession() = default;

    private:
        friend class StaticNotecard;
        using OpGuard = detail::StaticNcOpGuard<Lock>;
        OpGuard guard_;
        explicit ExclusiveSession(Lock* lock) : guard_(lock) {}
    };

    /// RAII guard returned by keep_ready(). Holds the RTX/CTX readiness
    /// scope for its lifetime; non-copyable, non-movable. Trivial empty
    /// guard when NOTE_TXN_HANDSHAKE is off.
    struct ReadySession {
        ReadySession(const ReadySession&)            = delete;
        ReadySession& operator=(const ReadySession&) = delete;
        ReadySession(ReadySession&&)                 = delete;
        ReadySession& operator=(ReadySession&&)      = delete;
        ~ReadySession() = default;

    private:
        friend class StaticNotecard;
#if NOTE_TXN_HANDSHAKE
        using TxnGuard = detail::StaticNcTxnOpGuard<Stack, Lock>;
        TxnGuard guard_;
        explicit ReadySession(StaticNotecard& nc) : guard_(nc) {}
#else
        explicit ReadySession(StaticNotecard&) {}
#endif
    };

    /// Hold the compile-time lock across a group of requests (exclusion only).
    /// Zero-cost no-op when Lock=NullLock. See Notecard::exclusive() for details.
    [[nodiscard]] ExclusiveSession exclusive() {
        return ExclusiveSession{static_cast<Lock*>(this)};
    }

    /// Hold the RTX/CTX readiness scope across a group of requests (readiness
    /// only). Zero-cost no-op when NOTE_TXN_HANDSHAKE is off. See
    /// Notecard::keep_ready() for details.
    [[nodiscard]] ReadySession keep_ready() {
        return ReadySession{*this};
    }

    /// One-shot `echo` connectivity probe. Mirror of `Notecard::ping()` —
    /// see the description there for the wire shape, timing, error
    /// semantics, and the meaning of `seed_fn`. The two implementations
    /// are deliberately kept in sync so the user-facing surface is the
    /// same on both Notecard variants.
    Result<void> ping(uint32_t timeout_ms = 500, PingSeedFn seed_fn = nullptr) {
        OpGuard op_{static_cast<Lock*>(this)};
        TxnOpGuard txn_op_{*this};
        uint32_t seed = (seed_fn ? seed_fn() : stack_.transport.hal().millis()) ^ 0x2545F491u;

        char nonce[16];
        for (int i = 0; i < 16; ++i) {
            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;
            nonce[i] = static_cast<char>('A' + (seed % 26));
        }

        constexpr char kPrefix[] = R"({"req":"echo","text":")";
        constexpr char kSuffix[] = R"("})";
        constexpr size_t kPrefixLen = sizeof(kPrefix) - 1;
        constexpr size_t kSuffixLen = sizeof(kSuffix) - 1;
        char req[kPrefixLen + 16 + kSuffixLen];
        memcpy(req, kPrefix, kPrefixLen);
        memcpy(req + kPrefixLen, nonce, 16);
        memcpy(req + kPrefixLen + 16, kSuffix, kSuffixLen);

#if !NOTE_NO_RETRY
        enforce_timing();
#endif
        char rsp_buf[64];
        auto rv = stack_.transport.transact_raw(string_view(req, sizeof(req)),
                                                rsp_buf, sizeof(rsp_buf),
                                                timeout_ms);
#if !NOTE_NO_RETRY
        record_timing();
#endif
        if (!rv) return Unexpected(rv.error());

        string_view rsp = *rv;
        constexpr string_view key = R"("text":")";
        auto pos = rsp.find(key);
        if (pos == string_view::npos)
            return make_error(Error::Json, NOTE_ERR("ping: response missing text field"));
        pos += key.size();
        if (pos + 16 > rsp.size())
            return make_error(Error::Json, NOTE_ERR("ping: response text too short"));
        if (memcmp(rsp.data() + pos, nonce, 16) != 0)
            return make_error(Error::Json, NOTE_ERR("ping: nonce mismatch"));
        return Result<void>{};
    }

private:
    using InplaceBuilder = void(*)(JsonRender& w, const void* ctx);

    /// Out-of-line workhorse for `transact_raw_inplace`. Non-template so
    /// it lives once per `StaticNotecard<Stack>` instantiation regardless
    /// of how many distinct lambdas the user passes.
    Result<string_view> transact_raw_inplace_impl_(char* buf, size_t cap,
                                                    InplaceBuilder fn,
                                                    const void* ctx,
                                                    uint32_t timeout_ms) {
        JsonRender w(buf, cap);
        fn(w, ctx);
        w.close();
        if (w.overflow())
            return make_error(Error::Overflow, Cause::Unspecified,
                              NOTE_ERR("transact_raw_inplace: buffer overflow"));
        const size_t req_size = w.size();
        return stack_.transport.transact_raw(string_view(buf, req_size),
                                             buf, cap, timeout_ms);
    }

public:

    /// Returns the next request ID if enabled, else 0. Mirror of
    /// `Notecard::next_request_id_or_zero` for the Api singleton-thunk
    /// path.
    uint32_t next_request_id_or_zero() {
#if !NOTE_NO_REQUEST_IDS
        return request_ids_enabled_ ? next_request_id_++ : 0;
#else
        return 0;
#endif
    }

#if !NOTE_MINIMAL
    /// Mirror of `Notecard::stash_nc_err` for the Api singleton-thunk
    /// path. Allocates the bytes from the Notecard's configured
    /// allocator (heap-leaked — same pattern as StringPool::intern; the
    /// allocator's lifetime carries the bytes). No fixed buffer: arena
    /// builds pay only for the messages they actually receive, and
    /// zero-allocator builds get nullptr (caller falls back to the
    /// caller-stack `NcErrorCapture::buf`, dangling past execute() —
    /// the same lifetime quirk that existed before this fix).
    /// Gated out under NOTE_MINIMAL: the singleton thunk on AVR-class
    /// targets doesn't call this, so the function would otherwise sit
    /// unused; gating ensures the linker doesn't accidentally retain it.
    const char* stash_nc_err(string_view sv) {
        if (sv.empty()) return nullptr;
        auto* p = static_cast<char*>(alloc_.allocate(sv.size()));
        if (!p) return nullptr;
        for (size_t i = 0; i < sv.size(); ++i) p[i] = sv[i];
        return p;
    }
#endif

    /// Non-template transact with retry. Wraps transact_dispatch in retry_loop.
    Result<void> transact_retried(BuildFn build_fn, void* build_ctx,
                                  SaxDispatch dispatch,
                                  detail::NcErrorCapture& nc_err,
                                  [[maybe_unused]] Safety safety) {
#if !NOTE_NO_RETRY
        enforce_timing();
#endif
        BuildFnRequestSource src(build_fn, build_ctx);
        auto rv = stack_.transport.transact_dispatch(src.as_source(), dispatch, default_timeout_ms_, nc_err);
#if !NOTE_NO_RETRY
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
                    BuildFnRequestSource retry_src(x.fn, x.build_ctx);
                    *x.rv = x.self->stack_.transport.transact_dispatch(
                        retry_src.as_source(), x.dispatch, x.self->default_timeout_ms_, *x.nc_err);
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
        detail::ReqWrapCtx ctx{req_type, fields_fn, fields_ctx};
        StringPool pool(alloc_);
        GenericResponseSink gsink{rsp_storage, rsp_fields, n_fields, &pool};
        if (body_handler) gsink.set_body_handler(body_handler);
        auto dispatch = make_sax_dispatch(gsink);
        auto rv = transact_retried(&detail::req_wrap_build, &ctx, dispatch, nc_err, safety);
        arena_exhausted = pool.exhausted();
        return rv;
    }

private:
    /// Operation-scope chokepoint mirroring the polymorphic Notecard path.
    ///
    /// Acquires the Lock (via the private base) unconditionally first; a RAII
    /// Exit guard releases it on any return path, including exceptions.
    ///
    /// The Lock template parameter MUST be recursive: same-thread nested
    /// entry points (e.g. execute() calling execute_void()) re-acquire the
    /// lock on the same thread, and a non-recursive lock would deadlock. For
    /// NullLock (the default), detail::StaticNcOpGuard specializes to zero
    /// members + trivial ctor/dtor, so the compiler eliminates the entire
    /// chokepoint — even on AVR with GCC 7.3 (which a pointer-carrying guard
    /// would not fully erase).
    using OpGuard = detail::StaticNcOpGuard<Lock>;
    using TxnOpGuard = detail::StaticNcTxnOpGuard<Stack, Lock>;
    friend struct detail::StaticNcTxnOpGuard<Stack, Lock>;

#if !NOTE_NO_RETRY
    RetryTransportOps transport_ops() {
        return {
            &stack_,
            [](void* c) -> uint32_t { return static_cast<Stack*>(c)->transport.hal().millis(); },
            [](void* c, uint32_t ms) { static_cast<Stack*>(c)->transport.hal().delay(ms); },
            [](void* c) { static_cast<Stack*>(c)->transport.reset(); },
        };
    }

    void enforce_timing() {
        if (!timing_.has_previous) return;
        uint32_t elapsed = stack_.transport.hal().millis() - timing_.last_transaction_end_ms;
        if (elapsed < timing_.min_gap_ms)
            stack_.transport.hal().delay(timing_.min_gap_ms - elapsed);
    }

    void record_timing() {
        timing_.last_transaction_end_ms = stack_.transport.hal().millis();
        timing_.has_previous = true;
    }
#endif

    /// Copy `alloc_` into `note::detail::g_singleton_allocator` under
    /// `NOTE_SINGLETON=1`. Captured by value so the global survives any
    /// StaticNotecard moves or factory patterns. No-op when SINGLETON
    /// is disabled. See the Notecard counterpart for the two consumers
    /// (Response dtor under RAII, streaming-path discriminator under
    /// both RAII and NO_RAII).
    void publish_singleton_allocator_() {
#if NOTE_SINGLETON
        ::note::detail::g_singleton_allocator = alloc_;
        ::note::detail::g_singleton_allocator_present = true;
#endif
    }

    Stack stack_;
    Allocator alloc_;
    uint32_t default_timeout_ms_ = 10000;
#if !NOTE_NO_RETRY
    RetryPolicy retry_policy_{};
    TransactionTiming timing_{};
#endif
#if !NOTE_NO_REQUEST_IDS
    uint32_t next_request_id_ = 1;
    bool request_ids_enabled_ = true;
#endif
#if NOTE_TXN_HANDSHAKE
    int op_depth_ = 0;  ///< Nesting depth for outermost-operation detection (gated so AVR pays nothing).
#endif
};

} // namespace note
