#pragma once

#include "allocator.hpp"
#include "binary_request.hpp"
#include "bus_lock.hpp"
#include "debug.hpp"
#include "owned_buffer.hpp"
#include "json.hpp"
#include "json_render.hpp"
#include "md5.hpp"
#include "response_release.hpp"
#include "retry.hpp"
#include "retry_policy.hpp"
#include "safety.hpp"
#include "span.hpp"
#include "protocol.hpp"
#include "string_pool.hpp"
#include "struct_sink.hpp"
#include "transact.hpp"
#include "lexer/parse.hpp"
#include "link/cobs.hpp"

#include <optional>
#include <type_traits>

// Response buffer + debug wire-capture buffer sizing. Both default to
// 1 KB; override via `-DNOTE_RSP_BUF_SIZE=N` / `-DNOTE_DBG_WIRE_BUF_SIZE=N`.
// Declared up here so all Notecard methods can use them; the storage
// itself is allocated below in the class body.
#ifndef NOTE_RSP_BUF_SIZE
#define NOTE_RSP_BUF_SIZE 1024
#endif
// Stack-allocated scratch for on_wire debug capture (per execute_tree
// call). Only consumed when debug.on_wire is set; otherwise the
// allocation is purely stack reservation. Sized to match RSP_BUF so
// the typical response fits.
#ifndef NOTE_DBG_WIRE_BUF_SIZE
#define NOTE_DBG_WIRE_BUF_SIZE NOTE_RSP_BUF_SIZE
#endif

namespace note {

// Forward declaration so generated request headers can `friend struct
// ::note::test::JsonbWireProbe;` without making the test header visible
// to production builds. The probe is defined in tests/common/jsonb_request_builder.hpp.
namespace test { struct JsonbWireProbe; }

/// Optional seed-supplying function for `Notecard::ping()` /
/// `StaticNotecard::ping()`. The probe's 16-character nonce is derived
/// from this seed via an internal xorshift32 PRNG; passing `nullptr`
/// (the default) seeds from the HAL clock (`hal().millis()`), which is
/// what production callers want. Tests inject a deterministic seed
/// function so consecutive nonces are predictable.
using PingSeedFn = uint32_t(*)();


#if NOTE_SINGLETON
namespace detail {
/// Shared NcT* (type-erased to void*) for all `request_traits<T>` under
/// NOTE_SINGLETON=1. Set by `Api<NcT>::create_<T>()` before each request
/// dispatch; read by the generated `execute()` / `command()` thunks.
///
/// Replaces a per-T `request_traits<T>::nc_` slot — saved (N − 1) ×
/// sizeof(void*) bytes of BSS in firmware that uses N distinct request
/// types through the Api groups.
inline void* api_nc_singleton_ = nullptr;
} // namespace detail
#endif

// Forward declarations so generated request structs can grant friendship
// to dispatch infrastructure that lives in other headers.
template<typename Stack, typename Lock> class StaticNotecard;
class Notecard;
#if NOTE_NO_POLYMORPHIC || __cplusplus < 202002L
template<typename NcT> class Api;
#else
template<typename TargetT, typename NcT> class Api;
#endif

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

    template<typename T, typename = void>
    struct has_alloc_ref : std::false_type {};
    template<typename T>
    struct has_alloc_ref<T, std::void_t<decltype(std::declval<T>().alloc_)>> : std::true_type {};

    /// Attach the Allocator value to the Response (by value — see
    /// AllocatorRef in response_release.hpp for why we don't pointer-track
    /// the Notecard's storage) so its destructor can free interned string
    /// fields. Only fires on Response types whose codegen emitted the
    /// `alloc_` member (i.e. those that own at least one string field or
    /// string-array field).
    template<typename T>
    void attach_allocator(T& rsp, const Allocator& a) {
        if constexpr (has_alloc_ref<T>::value) {
            rsp.alloc_.reset(a);
        } else {
            (void)rsp; (void)a;
        }
    }

    /// Per-request-type metadata for runtime dispatch. Specialized by codegen
    /// only for endpoints whose Response has simple (scalar / string / string-array)
    /// fields — for those we ship a FieldDesc table + count and the generic
    /// thunk path uses it. Lives outside the request struct so IntelliSense
    /// completion on `req.<TAB>` doesn't surface dispatch plumbing.
    template<typename T>
    struct request_traits;

    template<typename T, typename = void>
    struct has_field_descs : std::false_type {};
    template<typename T>
    struct has_field_descs<T, std::void_t<decltype(request_traits<T>::field_descs_ptr())>> : std::true_type {};

    /// JsonSink that watches for the top-level `"body"` object and forwards
    /// every nested SAX event to a BodyHandler. Used by the buffered execute
    /// path so `.into(T&)` populates the user's struct independently of
    /// transport — same contract the streaming Rsp::Sink provides inline.
    struct BodyDispatchSink : JsonSink {
        BodyHandler handler;
        int body_depth = 0;

        /// Construct with a non-empty handler. Only callers that have a
        /// real factory should instantiate this — see execute_tree for
        /// the `optional<BodyDispatchSink>` pattern that enforces it.
        explicit BodyDispatchSink(BodyHandler h) : handler(h) {}

        void on_object_begin(string_view k) override {
            if (body_depth > 0) {
                ++body_depth;
                handler.send(BodyEvent::make_object_begin(k));
                return;
            }
            if (detail::flash_key_eq(k, detail::common_keys::body)) body_depth = 1;
        }
        void on_object_end(string_view k) override {
            if (body_depth > 0) {
                --body_depth;
                if (body_depth > 0) handler.send(BodyEvent::make_object_end(k));
            }
        }
        void on_array_begin(string_view k) override {
            if (body_depth > 0) handler.send(BodyEvent::make_array_begin(k));
        }
        void on_array_end(string_view k) override {
            if (body_depth > 0) handler.send(BodyEvent::make_array_end(k));
        }
        void on_string(string_view k, string_view v) override {
            if (body_depth > 0) handler.send(BodyEvent::make_string(k, v));
        }
        void on_number(string_view k, string_view raw) override {
            if (body_depth > 0) handler.send(BodyEvent::make_number(k, raw));
        }
        void on_bool(string_view k, bool v) override {
            if (body_depth > 0) handler.send(BodyEvent::make_bool(k, v));
        }
        void on_null(string_view k) override { (void)k; }
        void reset() override {
            body_depth = 0;
            handler.send(BodyEvent::make_reset());
        }
    };
}


// Specialization for void responses (endpoints that return empty {} on success).
// Still holds a reader to keep notecard error message string_views alive.
template<>
class ApiResult<void> {
    std::optional<ErrorInfo> err_;
#if !NOTE_NO_JSON_TREE
    std::unique_ptr<JsonReader> reader_;
#endif
public:
    ApiResult() = default;
    ApiResult(ErrorInfo e) : err_(std::move(e)) {}
#if !NOTE_NO_JSON_TREE
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

#if !NOTE_NO_POLYMORPHIC
class Notecard {
public:
    Notecard() { publish_singleton_allocator_(); }

    /// Streaming ctor — preferred for new code. Responses are SAX-parsed
    /// off the wire; typed fields (`r.version`, `.into(struct&)`) work
    /// without any JSON tree-mode path linked. `body()` returns nullptr
    /// in this mode; `body_or_error()` returns an explicit error.
    explicit Notecard(ITransact& transport, Allocator alloc = {})
        : transport_(&transport)
        , alloc_(alloc)
    { publish_singleton_allocator_(); }

    /// Tree-mode ctor: a JsonBackend builds a walkable response tree so
    /// `Response::body()` returns a JsonReader for ad-hoc field access.
    /// Streaming is the recommended default unless you need `body()` or
    /// the lambda request builder; pass the single-arg streaming ctor
    /// above otherwise.
    Notecard(JsonBackend& backend, ITransact& transport)
        : backend_(&backend)
        , transport_(&transport)
    { publish_singleton_allocator_(); }

    /// Unified ctor (Phase 5a step 8a). `backend` may be nullptr — pass
    /// nullptr + a non-null allocator for streaming-only mode; pass a
    /// backend to enable the JSON tree-mode parse path. The allocator
    /// selects the response-strategy: when set, execute() drives the
    /// streaming SAX path; when unset and a backend is present, the
    /// tree-parse path runs.
    Notecard(JsonBackend* backend, ITransact& transport, Allocator alloc = {})
        : backend_(backend)
        , transport_(&transport)
        , alloc_(alloc)
    { publish_singleton_allocator_(); }

    /// Protocol-typed ctors. `Protocol` exposes the send/read split that
    /// `transact(string_view) -> OwnedBuffer` uses for the growable
    /// byte-by-byte response path; the unified ITransact ctors above
    /// only get the bounded `transact(req, span<char>, t)` path. Pass a
    /// `Protocol&` here to opt into the growable response.
    Notecard(Protocol& transport, Allocator alloc = {})
        : Notecard(nullptr, static_cast<ITransact&>(transport), alloc)
    {
        streaming_protocol_ = &transport;
    }

    Notecard(JsonBackend* backend, Protocol& transport, Allocator alloc = {})
        : Notecard(backend, static_cast<ITransact&>(transport), alloc)
    {
        streaming_protocol_ = &transport;
    }

    // Configure an allocator for response string interning.
    // When set, execute() copies response string_view fields into the
    // allocator's backing store (e.g. a MonotonicArena) so they survive
    // transport buffer reuse. The parsing strategy is still dictated by
    // the backend (tree-parse or SAX).
    void set_allocator(Allocator alloc) { alloc_ = alloc; publish_singleton_allocator_(); }
    void clear_allocator() { alloc_.reset(); publish_singleton_allocator_(); }

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

    // Override the response staging buffer used by buffered execute paths
    // (`request()`, `execute_tree()`, `transact(json) -> OwnedBuffer`).
    // By default Notecard uses an internal NOTE_RSP_BUF_SIZE-byte buffer
    // (1024 bytes); supply a larger / smaller span for a particular
    // Notecard instance if the default doesn't fit your largest expected
    // response. The caller owns the buffer and must keep it alive for the
    // lifetime of any in-flight transaction.
    void set_response_buffer(span<char> buf) { rsp_buf_override_ = buf; }
    void set_response_buffer(char* buf, size_t len) { rsp_buf_override_ = {buf, len}; }
    template<size_t N>
    void set_response_buffer(char (&buf)[N]) { rsp_buf_override_ = {buf, N}; }

    /// Serialize whole Notecard operations against other threads sharing this
    /// Notecard instance. The lock is held across the entire operation
    /// (including multi-step binary transfers). Null by default = no locking,
    /// zero behavioral cost.
    ///
    /// **The lock must be recursive** (e.g. `std::recursive_mutex`, or a
    /// recursive RTOS mutex). Nested entry points — such as execute() called
    /// from within do_binary_send — re-acquire the lock on the same thread;
    /// a non-recursive lock would deadlock. (The bus lock set via
    /// `set_bus_lock` on the transport does not need to be recursive.)
    void set_request_lock(IBusLock& l) { request_lock_ = &l; }
    void clear_request_lock() { request_lock_ = nullptr; }

    // Execute a typed, generated request.
    // RequestT must provide:
    //   static constexpr string_view notecard_request;
    //   static constexpr bool supports_cmd;
    //   static constexpr Safety safety;
    //   using Response = ...;
    //   void build(JsonBuilder&) const;
    template<typename RequestT>
    ApiResult<typename RequestT::Response> execute(const RequestT& req) {
        return run_operation([&]() -> ApiResult<typename RequestT::Response> {
            using Rsp = typename RequestT::Response;
            constexpr bool can_stream =
                std::is_void_v<Rsp> || detail::has_sink<Rsp>::value;

            // Type-independent prologue (timing + path selection) — see
            // begin_execute(). Only the type-dependent calls below stay here.
            ExecDecision d = begin_execute(can_stream, RequestT::notecard_request);

            ApiResult<Rsp> result = [&]() -> ApiResult<Rsp> {
                // Streaming path: SAX-stream the response straight into the
                // typed Rsp::Sink, no intermediate tree.
                if constexpr (can_stream) {
                    if (d.path == ExecPath::Streaming) {
                        return execute_streaming(req);
                    }
                }
                // Buffered fallback: JsonBackend + buffered transport, retried.
#if !NOTE_NO_JSON_TREE
                if (d.path == ExecPath::Buffered) {
                    auto attempt = [&]() -> ApiResult<Rsp> {
                        return execute_tree(req, d.req_id);
                    };
                    auto reset = [&]() { transport_->reset(); };
                    return retry_transaction<ApiResult<Rsp>>(
                        hal(), timing_, RequestT::safety, retry_policy_,
                        attempt, reset);
                }
#endif
                return *d.error;
            }();

            end_execute(RequestT::notecard_request);
            return result;
        });
    }

    // ── Binary transfer support ────────────────────────────────────────────
    //
    // Requests with .data() or .into() carry binary buffers. execute()
    // on a non-const request checks for attached buffers and handles
    // COBS encode/decode transparently.

    /// Execute a mutable request — checks for attached binary buffers.
    template<typename RequestT>
    ApiResult<typename RequestT::Response> execute(RequestT& req) {
        return run_operation([&]() -> ApiResult<typename RequestT::Response> {
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
        });
    }

#if !NOTE_SINGLETON && !NOTE_NO_RESPONSE_RAII
    // Execute with an explicit allocator (one-off string interning).
    //
    // Gated out under NOTE_SINGLETON=1: Response cleanup under SINGLETON
    // resolves the allocator through a single global slot instead of a
    // per-Response copy, and the swap-and-restore done by this overload
    // can't be reconciled with that scheme when Responses outlive the
    // call.
    //
    // Gated out under NOTE_NO_RESPONSE_RAII=1: the whole point of the
    // overload is "this Response is owned by a different allocator than
    // the Notecard's configured one." Without per-Response cleanup that
    // contract no longer holds — the caller would have to track the
    // temp allocator's lifetime against every parsed Response themselves.
    // Callers that need per-call allocators should use the default
    // (non-singleton, RAII-enabled) build.
    template<typename RequestT>
    ApiResult<typename RequestT::Response> execute(const RequestT& req, Allocator alloc) {
        return run_operation([&]() -> ApiResult<typename RequestT::Response> {
            auto saved = alloc_;
            alloc_ = alloc;
            auto result = execute(req);  // intentionally re-entrant (op_depth_ already > 0)
            alloc_ = saved;
            return result;
        });
    }
#endif

#if !NOTE_NO_STD_STRING
    // Ad-hoc request with a builder callback.
    // Requires std::function and a buffered transport + backend.
    Result<std::unique_ptr<JsonReader>> request(
            string_view req_type,
            std::function<void(JsonBuilder&)> build_fn = {}) {
        return run_operation([&]() -> Result<std::unique_ptr<JsonReader>> {
            if (!transport_ || !backend_)
                return make_error(Error::NotReady, NOTE_ERR("no buffered transport configured"));

            auto& builder = backend_->get_builder();
            add_flash(builder, flash(detail::common_keys::req), req_type);
            if (build_fn) build_fn(builder);
            auto rsp = transport_->transact(builder.to_view(),
                                            rsp_buf(),
                                            default_timeout_ms_);
            if (!rsp) return Unexpected(rsp.error());

            auto reader = backend_->parse_response(*rsp);
            if (reader->has_error()) {
                return make_error(Error::Json, reader->get_error());
            }
            return Result<std::unique_ptr<JsonReader>>(std::move(reader));
        });
    }

#endif // !NOTE_NO_STD_STRING && !NOTE_NO_STD_FUNCTION

    /// Type-erased send (fire-and-forget). Used by generated command() methods
    /// via send_fn_ — a single shared function pointer for all request types.
    ///
    /// When an allocator is configured, builds the request through
    /// `BuildFnRequestSource` (streams directly to the wire). Otherwise
    /// falls back to backend-builder + string_view send so the buffered
    /// path's exact wire encoding (used by tests' `last_req` capture) is
    /// preserved.
    Result<void> send_command(BuildFn build_fn, void* ctx) {
        return run_operation([&]() -> Result<void> {
            if (!transport_)
                return make_error(Error::NotReady, NOTE_ERR("no transport configured"));
            enforce_timing();
            Result<void> result;
            if (alloc_.has_value()) {
                BuildFnRequestSource src(build_fn, ctx);
                result = transport_->send(src.as_source());
            }
#if !NOTE_NO_JSON_TREE
            else if (backend_) {
                auto& builder = backend_->get_builder();
                build_fn(builder, ctx);
                result = transport_->send(builder.to_view());
            }
#endif
            else
                return make_error(Error::NotReady, NOTE_ERR("no allocator or backend configured"));
            record_timing();
            return result;
        });
    }

    // ── Singleton-thunk adapters ───────────────────────────────────────
    //
    // The Api singleton-thunk machinery (`Api::void_thunk_` / `generic_thunk_`
    // in note/api.hpp) calls these on whatever `NcT` it was instantiated
    // with. StaticNotecard exposes them natively; here we adapt them onto
    // the polymorphic Notecard so `-DNOTE_SINGLETON=1` (without
    // `NOTE_STATIC_HAL=1`) compiles and runs against the dynamic-HAL
    // Notecard.
    //
    // Two paths, mirroring the regular `execute()` template:
    //   (1) streaming — when an allocator is configured, frame the request
    //       through `RequestFrame` + `streaming_execute` and SAX-parse the
    //       response sink-side.
    //   (2) buffered  — when only a JsonBackend is configured, build into
    //       the backend's builder, `transact(string_view, buf, …)`, then
    //       SAX-parse the response buffer for fields.
    //
    // Error model translation: both paths produce a `Notecard "err"`
    // string. We funnel it into the caller's `NcErrorCapture` so the
    // contract matches StaticNotecard's. Other errors propagate as
    // Unexpected.
    //
    // No request-id injection — StaticNotecard's singleton path doesn't
    // emit one either; the singleton machinery is currently id-less by
    // design.
    Result<void> execute_void(string_view req_type, BuildFn fields_fn, void* fields_ctx,
                              detail::NcErrorCapture& nc_err,
                              [[maybe_unused]] Safety safety = Safety::NonIdempotent) {
        return run_operation([&]() -> Result<void> {
            if (!transport_)
                return make_error(Error::NotReady, NOTE_ERR("no transport configured"));

            if (alloc_.has_value()) {
                RequestFrame frame{fields_fn, fields_ctx, req_type, 0};
                JsonSink null_sink;
                auto ei = streaming_execute(frame, null_sink, safety, nullptr, nullptr);
                if (ei.code == Error::Notecard) {
                    nc_err.capture(string_view{ei.message.data(), ei.message.size()});
                    return {};
                }
                if (ei.code != Error{}) return Unexpected(ei);
                return {};
            }

#if !NOTE_NO_JSON_TREE
            if (backend_) {
                debug_timing(debug_, TimingEvent::TransactionBegin, req_type);
                enforce_timing();
                debug_timing(debug_, TimingEvent::BuildBegin, req_type);
                auto& builder = backend_->get_builder();
                add_flash(builder, flash(detail::common_keys::req), req_type);
                fields_fn(builder, fields_ctx);
                auto req_json = builder.to_view();
                debug_timing(debug_, TimingEvent::BuildEnd, req_type);
                debug_wire(debug_, req_json, WireDirection::Send);
                auto rsp = transport_->transact(req_json, rsp_buf(),
                                                default_timeout_ms_);
                record_timing();
                debug_timing(debug_, TimingEvent::TransactionEnd, req_type);
                if (!rsp) return Unexpected(rsp.error());
                debug_wire(debug_, *rsp, WireDirection::Receive);
                auto& reader = backend_->get_reader(*rsp);
                if (reader.has_error())
                    return make_error(Error::Json, NOTE_ERR("JSON parse error"));
                auto err = reader.get_error();
                if (!err.empty()) nc_err.capture(err);
                return {};
            }
#endif
            return make_error(Error::NotReady, NOTE_ERR("no allocator or backend configured"));
        });
    }

    Result<void> execute_generic_with_body(
            string_view req_type, BuildFn fields_fn, void* fields_ctx,
            void* rsp_storage, const FieldDesc* rsp_fields, uint8_t n_fields,
            detail::NcErrorCapture& nc_err, bool& arena_exhausted,
            void* body_ptr, BodyHandlerFactory body_factory,
            Safety safety = Safety::NonIdempotent) {
        return run_operation([&]() -> Result<void> {
            if (!transport_)
                return make_error(Error::NotReady, NOTE_ERR("no transport configured"));

            if (alloc_.has_value()) {
                StringPool pool(*alloc_);
                GenericResponseSink gsink{rsp_storage, rsp_fields, n_fields, &pool};
                alignas(body_sink_storage_align) char body_storage[body_sink_storage_size];
                if (body_factory) {
                    auto bh = body_factory(body_ptr, pool, body_storage);
                    if (bh) gsink.set_body_handler(bh);
                }
                JsonSinkAdapter<GenericResponseSink> sink_adapter(gsink);

                RequestFrame frame{fields_fn, fields_ctx, req_type, 0};
                auto ei = streaming_execute(frame, sink_adapter, safety,
                                            nullptr, nullptr);
                arena_exhausted = pool.exhausted();
                if (ei.code == Error::Notecard) {
                    nc_err.capture(string_view{ei.message.data(), ei.message.size()});
                    return {};
                }
                if (ei.code != Error{}) return Unexpected(ei);
                return {};
            }

#if !NOTE_NO_JSON_TREE
            if (backend_) {
                debug_timing(debug_, TimingEvent::TransactionBegin, req_type);
                enforce_timing();
                debug_timing(debug_, TimingEvent::BuildBegin, req_type);
                auto& builder = backend_->get_builder();
                add_flash(builder, flash(detail::common_keys::req), req_type);
                fields_fn(builder, fields_ctx);
                auto req_json = builder.to_view();
                debug_timing(debug_, TimingEvent::BuildEnd, req_type);
                debug_wire(debug_, req_json, WireDirection::Send);
                auto rsp = transport_->transact(req_json, rsp_buf(),
                                                default_timeout_ms_);
                record_timing();
                debug_timing(debug_, TimingEvent::TransactionEnd, req_type);
                if (!rsp) return Unexpected(rsp.error());
                debug_wire(debug_, *rsp, WireDirection::Receive);
                auto& reader = backend_->get_reader(*rsp);
                if (reader.has_error())
                    return make_error(Error::Json, NOTE_ERR("JSON parse error"));
                auto err = reader.get_error();
                if (!err.empty()) {
                    nc_err.capture(err);
                    return {};
                }

                StringPool pool(alloc_value());
                GenericResponseSink gsink{rsp_storage, rsp_fields, n_fields, &pool};
                alignas(body_sink_storage_align) char body_storage[body_sink_storage_size];
                if (body_factory) {
                    auto bh = body_factory(body_ptr, pool, body_storage);
                    if (bh) gsink.set_body_handler(bh);
                }
                // Use the lexer-based `sax_lex` (not `sax_parse`): it emits
                // typed `on_int` / `on_float` / `on_bool` events that
                // `GenericResponseSink` dispatches into the field table.
                // The buffer-based `sax_parse` only emits `on_number` (raw
                // string), which `GenericResponseSink` only forwards to
                // body handlers — top-level scalar fields silently drop.
                JsonSinkAdapter<GenericResponseSink> sink_adapter(gsink);
                sax_lex(*rsp, sink_adapter);
                arena_exhausted = pool.exhausted();
                return {};
            }
#endif
            return make_error(Error::NotReady, NOTE_ERR("no allocator or backend configured"));
        });
    }

    template<typename RequestT>
    Result<void> command_typed(const RequestT& req) {
        return run_operation([&]() -> Result<void> {
            if (!transport_)
                return make_error(Error::NotReady, NOTE_ERR("no transport configured"));
            enforce_timing();
            Result<void> result;
            if (alloc_.has_value()) {
                auto build = [&](JsonBuilder& b) {
                    add_flash(b, flash(detail::common_keys::cmd), RequestT::notecard_request);
                    req.build(b);
                };
                BuildFn fn = [](JsonBuilder& b, void* p) {
                    (*static_cast<decltype(build)*>(p))(b);
                };
                BuildFnRequestSource src(fn, &build);
                result = transport_->send(src.as_source());
            } else if (backend_) {
                auto& builder = backend_->get_builder();
                add_flash(builder, flash(detail::common_keys::cmd), RequestT::notecard_request);
                req.build(builder);
                result = transport_->send(builder.to_view());
            } else {
                return make_error(Error::NotReady, NOTE_ERR("no allocator or backend configured"));
            }
            record_timing();
            return result;
        });
    }

#if !NOTE_NO_STD_STRING
    // Fire-and-forget command with builder callback.
    // Requires std::function.
    Result<void> command(string_view cmd_type,
                         std::function<void(JsonBuilder&)> build_fn = {}) {
        return run_operation([&]() -> Result<void> {
            if (!transport_)
                return make_error(Error::NotReady, NOTE_ERR("no transport configured"));
            if (alloc_.has_value()) {
                auto build = [&](JsonBuilder& b) {
                    add_flash(b, flash(detail::common_keys::cmd), cmd_type);
                    if (build_fn) build_fn(b);
                };
                BuilderRequestSource src(build);
                return transport_->send(src.as_source());
            }
            if (!backend_)
                return make_error(Error::NotReady, NOTE_ERR("no allocator or backend configured"));

            auto& builder = backend_->get_builder();
            add_flash(builder, flash(detail::common_keys::cmd), cmd_type);
            if (build_fn) build_fn(builder);
            return transport_->send(builder.to_view());
        });
    }
#endif // !NOTE_NO_STD_STRING && !NOTE_NO_STD_FUNCTION

    /// Validated JSON passthrough — returns an OwnedBuffer that the caller owns.
    /// The buffer is freed when it goes out of scope. No dangling views.
    /// Assumes NonIdempotent safety (only retries on SendFailed).
    ///
    /// When the underlying transport is a `Protocol` and an allocator is
    /// configured, reads response bytes one at a time into a growable
    /// OwnedBuffer (no a-priori size cap). For other transports falls back
    /// to the bounded buffered path — bounded by `rsp_buf()` (default
    /// NOTE_RSP_BUF_SIZE), enlarge via `set_response_buffer(span)`.
    Result<OwnedBuffer> transact(string_view json) {
        return run_operation([&]() -> Result<OwnedBuffer> {
            if (!validate_json_envelope(json))
                return make_error(Error::Json, NOTE_ERR("malformed JSON"));
            if (!transport_)
                return make_error(Error::NotReady, NOTE_ERR("no transport configured"));

            if (streaming_protocol_ && alloc_.has_value()) {
                auto attempt = [&]() -> Result<OwnedBuffer> {
                    auto send_rv = streaming_protocol_->send_raw(json);
                    if (!send_rv) return Unexpected(send_rv.error());

                    auto buf = OwnedBuffer::create(alloc_value(), 1024);
                    if (!buf) return make_error(Error::Overflow, NOTE_ERR("alloc failed"));

                    for (;;) {
                        uint8_t byte;
                        auto rv = streaming_protocol_->read(&byte, 1, default_timeout_ms_);
                        if (!rv) return Unexpected(rv.error());
                        if (*rv == 0) return make_error(Error::ResponseLost, Cause::Timeout, NOTE_ERR("response timeout"));
                        if (byte == '\n') break;
                        if (byte == '\r') continue;
                        if (!buf.append(static_cast<char>(byte)))
                            return make_error(Error::Overflow, NOTE_ERR("response exceeds available memory"));
                    }
                    buf.null_terminate();
                    return buf;
                };
                auto reset = [&]() { transport_->reset(); };
                return retry_transaction<Result<OwnedBuffer>>(
                    hal(), timing_, Safety::NonIdempotent, retry_policy_,
                    attempt, reset);
            }

            auto attempt = [&]() -> Result<OwnedBuffer> {
                auto rv = transport_->transact(json,
                                               rsp_buf(),
                                               default_timeout_ms_);
                if (!rv) return Unexpected(rv.error());
                auto buf = OwnedBuffer::create(alloc_value(), rv->size() + 1);
                if (!buf) return make_error(Error::Overflow, NOTE_ERR("alloc failed"));
                buf.append(rv->data(), rv->size());
                buf.null_terminate();
                return buf;
            };
            auto reset = [&]() { transport_->reset(); };
            return retry_transaction<Result<OwnedBuffer>>(
                hal(), timing_, Safety::NonIdempotent, retry_policy_,
                attempt, reset);
        });
    }

    /// Validated JSON passthrough — caller-provided buffer variant.
    /// The response is written into buf and returned as string_view.
    /// Assumes NonIdempotent safety (only retries on SendFailed).
    Result<string_view> transact(string_view json, span<char> buf) {
        return run_operation([&]() -> Result<string_view> {
            if (!validate_json_envelope(json))
                return make_error(Error::Json, NOTE_ERR("malformed JSON"));
            if (!transport_)
                return make_error(Error::NotReady, NOTE_ERR("no transport configured"));

            auto attempt = [&]() -> Result<string_view> {
                // Caller's buffer goes straight to the transport; ITransact
                // copies the response into `buf` and returns a view of it.
                auto rv = transport_->transact(json, buf, default_timeout_ms_);
                if (!rv) return rv;
                if (rv->size() < buf.size())
                    buf[rv->size()] = '\0';
                return rv;
            };
            auto reset = [&]() { transport_->reset(); };
            return retry_transaction<Result<string_view>>(
                hal(), timing_, Safety::NonIdempotent, retry_policy_,
                attempt, reset);
        });
    }

    /// Validated JSON fire-and-forget — send pre-formatted JSON, no response.
    /// Inter-transaction timing enforced, but no retry (no response to check).
    Result<void> send(string_view json) {
        return run_operation([&]() -> Result<void> {
            if (!validate_json_envelope(json))
                return make_error(Error::Json, NOTE_ERR("malformed JSON"));
            if (!transport_)
                return make_error(Error::NotReady, NOTE_ERR("no transport configured"));
            enforce_timing();
            auto result = transport_->send(json);
            record_timing();
            return result;
        });
    }

    /// Send a one-shot `echo` probe and confirm the Notecard is reachable.
    ///
    /// The request is `{"req":"echo","text":"<16-char nonce>"}`. The
    /// Notecard echoes the nonce back in the response's `text` field;
    /// `ping()` succeeds only if the response nonce matches the request
    /// nonce byte-for-byte.
    ///
    /// This is intentionally a stripped-down transaction: a single
    /// attempt, a short fixed default timeout, no retry on failure, no
    /// CRC field, and no transport reset. The probe therefore stays
    /// safe to call at any point in the lifecycle, including before any
    /// other transaction has run.
    ///
    /// The 16-character nonce is generated by an xorshift32 PRNG seeded
    /// from `seed_fn()` (or, when null, from the HAL clock). Tests can
    /// inject a deterministic seed function to make consecutive nonces
    /// predictable; production callers should leave `seed_fn` at its
    /// default so consecutive pings carry different nonces.
    ///
    /// Returns success when the nonce matches; a transport error when
    /// the transport itself failed; or `Error::Json` when the response
    /// is missing a `text` field or its `text` value does not match the
    /// sent nonce.
    Result<void> ping(uint32_t timeout_ms = 500, PingSeedFn seed_fn = nullptr) {
        return run_operation([&]() -> Result<void> {
            if (!transport_)
                return make_error(Error::NotReady, NOTE_ERR("no transport configured"));

            uint32_t seed = (seed_fn ? seed_fn() : hal().millis()) ^ 0x2545F491u;

            char nonce[16];
            for (int i = 0; i < 16; ++i) {
                seed ^= seed << 13;
                seed ^= seed >> 17;
                seed ^= seed << 5;
                nonce[i] = static_cast<char>('A' + (seed % 26));
            }

            // Hand-build the request so we do not pull in snprintf on AVR.
            // Shape: {"req":"echo","text":"NNNN...NNNN"}
            constexpr char kPrefix[] = R"({"req":"echo","text":")";
            constexpr char kSuffix[] = R"("})";
            constexpr size_t kPrefixLen = sizeof(kPrefix) - 1;
            constexpr size_t kSuffixLen = sizeof(kSuffix) - 1;
            char req[kPrefixLen + 16 + kSuffixLen];
            memcpy(req, kPrefix, kPrefixLen);
            memcpy(req + kPrefixLen, nonce, 16);
            memcpy(req + kPrefixLen + 16, kSuffix, kSuffixLen);

            enforce_timing();
            char rsp_buf[64];
            auto rv = transport_->transact(string_view(req, sizeof(req)),
                                           span<char>(rsp_buf, sizeof(rsp_buf)),
                                           timeout_ms);
            record_timing();
            if (!rv) return Unexpected(rv.error());

            // The Notecard returns canonical JSON with no whitespace or
            // escapes, so a substring match on "text":"<nonce>" is enough
            // and avoids pulling in a SAX parser here.
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
        });
    }

    /// In-place variant — `buf` is used both to render the request *and* to
    /// receive the response. The lambda receives a writer (`auto& w`) with
    /// the same `add()` / `begin_object()` / `close()` shape as `JsonBuf`.
    /// After it returns, the rendered bytes are sent over the transport,
    /// then the response overwrites `buf`. The returned `string_view`
    /// points into `buf`.
    ///
    /// Mirrors `StaticNotecard::transact_raw_inplace` so the same call
    /// shape works on both Notecard variants. The shared buffer is safe
    /// because `ITransact::transact` fully drains the request before
    /// reading the response.
    ///
    ///     char buf[64];
    ///     auto v = note::JsonView(nc.transact_raw_inplace(buf, [](auto& w) {
    ///         w.add("req", "card.temp");
    ///     }));
    ///     float t = v.get_float("value");
    template<size_t N, typename Fn>
    Result<string_view> transact_raw_inplace(char (&buf)[N], Fn&& build,
                                             uint32_t timeout_ms = 10000) {
        return run_operation([&]() -> Result<string_view> {
            // Type-erase the lambda via a stateless trampoline so the bulk of
            // the work (`transact_raw_inplace_impl_`) is one out-of-line copy
            // shared by every call site.
            InplaceBuilder trampoline = [](JsonRender& w, const void* ctx) {
                (*static_cast<const std::remove_reference_t<Fn>*>(ctx))(w);
            };
            return transact_raw_inplace_impl_(buf, N, trampoline,
                                              static_cast<const void*>(&build),
                                              timeout_ms);
        });
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
        if (transport_)
            transport_->set_debug(d);
    }

    /// Disable all debug callbacks.
    void clear_debug() { set_debug({}); }

    /// Access the current debug listener.
    const DebugListener& debug() const { return debug_; }

    /// Access the underlying transport.
    ITransact& transport() { return *transport_; }

    /// Access the underlying byte HAL — for low-level timing, bus reset,
    /// and other operations that don't go through the wire protocol.
    /// Asserts in debug builds if no transport has been configured.
    Hal& hal() {
        return transport_->hal();
    }

    JsonBackend& backend() { return *backend_; }

    /// Returns the next request ID if request IDs are enabled, else 0.
    /// Singleton-thunk path uses this to inject IDs without changing
    /// `execute_void`'s signature: the thunk wraps the caller's BuildFn
    /// with `id_wrap_build` and passes the id alongside.
    uint32_t next_request_id_or_zero() {
        return request_ids_enabled_ ? next_request_id_++ : 0;
    }

    // ── Type-independent execute() core ─────────────────────────────────────
    // execute<RequestT>() is instantiated once per request type, so any branch
    // in its body is counted (and code-generated) once per type. The config
    // checks below (transport/streaming/buffered selection, the debug-timing
    // null checks) are identical across every RequestT, so they live here in
    // non-template members: emitted and measured once, not per instantiation.
    // Only the genuinely type-dependent calls (execute_streaming/execute_tree)
    // stay in the template. See DESIGN-branch-coverage-floor.md.
    enum class ExecPath : uint8_t { Streaming, Buffered, Error };
    struct ExecDecision {
        ExecPath               path = ExecPath::Error;
        uint32_t               req_id = 0;
        std::optional<Unexpected> error;   // set iff path == Error
    };

    // Prologue: emit begin-timing, validate config, choose the dispatch path.
    // `can_stream` is the compile-time streamability of the response type.
    ExecDecision begin_execute(bool can_stream, string_view req_name) {
        debug_timing(debug_, TimingEvent::TransactionBegin, req_name);
        ExecDecision d;
        if (!transport_) {
            d.error = make_error(Error::NotReady, NOTE_ERR("no transport configured"));
            return d;
        }
        if (can_stream && alloc_.has_value()) {
            d.path = ExecPath::Streaming;
            return d;
        }
#if !NOTE_NO_JSON_TREE
        if (backend_) {
            d.path = ExecPath::Buffered;
            d.req_id = next_request_id_or_zero();
            return d;
        }
#endif
        d.error = make_error(Error::NotReady, NOTE_ERR("no backend or streaming transport configured"));
        return d;
    }

    // Epilogue: emit end-timing. Called once per execute(), regardless of path.
    void end_execute(string_view req_name) {
        debug_timing(debug_, TimingEvent::TransactionEnd, req_name);
    }

    /// Allocator-backed durable copy of a Notecard error message — used
    /// by the Api singleton thunk path to give `NcErrorCapture::view()`
    /// a pointer that outlives the caller-stack-allocated NcErrorCapture
    /// frame. Without this, generated `execute()` returns an
    /// `ApiResult<...>` whose `ErrorMessage` points into dead stack
    /// memory by the time `result.error().message` is read.
    ///
    /// Allocates `sv.size()` bytes from the configured `alloc_` (one
    /// allocation per Notecard error; never freed — same heap-leaked
    /// pattern as `streaming_attempt`'s `pool.intern(err)` on the
    /// non-singleton path). Returns `nullptr` if no allocator is
    /// configured or the allocation fails — caller falls back to
    /// `NcErrorCapture::buf` (still dangling past execute(), but at
    /// least no fixed buffer is consumed for the rare no-allocator
    /// build).
    const char* stash_nc_err(string_view sv) {
        if (sv.empty() || !alloc_.has_value()) return nullptr;
        auto* p = static_cast<char*>(alloc_->allocate(sv.size()));
        if (!p) return nullptr;
        for (size_t i = 0; i < sv.size(); ++i) p[i] = sv[i];
        return p;
    }

    // ── Operation-session guards ──────────────────────────────────────────
    //
    // Two independent RAII guards an app holds across a GROUP of requests.
    //
    // exclusive() — holds the recursive request lock for the guard's lifetime
    //   so a group of requests is atomic against other threads (exclusion only).
    //   Inner run_operation() calls re-acquire the lock harmlessly (recursive).
    //   No-op when no request lock is configured.
    //
    // keep_ready() — holds the RTX/CTX readiness scope (op_depth_ /
    //   begin_operation / end_operation) for the guard's lifetime so the card
    //   is asserted-ready once for the whole group instead of per request
    //   (readiness only). Inner run_operation() calls see op_depth_ > 0 and
    //   skip begin_operation. When NOTE_TXN_HANDSHAKE is off, returns a trivial
    //   empty guard with no op_depth_ touch. No-op when no transport is set.
    //
    // Both guards: non-copyable, non-movable. C++17 guaranteed copy elision
    // (NRVO) makes `auto g = nc.exclusive();` work without any copy or move.
    //
    // The two guards are INDEPENDENT — an app may use either, both, or neither.
    // exclusive() = exclusion; keep_ready() = readiness. They compose correctly:
    //
    //     auto ex = nc.exclusive();   // lock the group
    //     auto kr = nc.keep_ready();  // hold RTX-ready for the group
    //     nc.execute(req1);
    //     nc.execute(req2);

    /// RAII guard returned by exclusive(). Holds the recursive request lock
    /// for its lifetime; non-copyable, non-movable.
    struct ExclusiveSession {
        ExclusiveSession(const ExclusiveSession&)            = delete;
        ExclusiveSession& operator=(const ExclusiveSession&) = delete;
        ExclusiveSession(ExclusiveSession&&)                 = delete;
        ExclusiveSession& operator=(ExclusiveSession&&)      = delete;

        ~ExclusiveSession() {
            if (lock_) lock_->unlock();
        }
    private:
        friend class Notecard;
        explicit ExclusiveSession(IBusLock* lock) : lock_(lock) {
            if (lock_) lock_->lock();
        }
        IBusLock* lock_;
    };

    /// RAII guard returned by keep_ready(). Holds the RTX/CTX readiness scope
    /// (op_depth_ / begin_operation / end_operation) for its lifetime so the
    /// card is asserted-ready once for the whole group; non-copyable, non-movable.
    /// When NOTE_TXN_HANDSHAKE is off this is a trivial empty struct.
    struct ReadySession {
        ReadySession(const ReadySession&)            = delete;
        ReadySession& operator=(const ReadySession&) = delete;
        ReadySession(ReadySession&&)                 = delete;
        ReadySession& operator=(ReadySession&&)      = delete;

#if NOTE_TXN_HANDSHAKE
        ~ReadySession() {
            if (outermost_ && nc_->transport_) nc_->transport_->end_operation();
            --nc_->op_depth_;
        }
#else
        ~ReadySession() = default;
#endif

    private:
        friend class Notecard;
#if NOTE_TXN_HANDSHAKE
        explicit ReadySession(Notecard* nc)
            : nc_(nc)
            , outermost_(nc_->op_depth_++ == 0)
        {
            if (outermost_ && nc_->transport_)
                nc_->transport_->begin_operation(nc_->default_timeout_ms_);
        }
        Notecard* nc_;
        bool outermost_;
#else
        explicit ReadySession(Notecard*) {}
#endif
    };

    /// Hold the recursive request lock across a group of requests, making the
    /// group atomic against other threads (exclusion only — does NOT affect
    /// the RTX/CTX readiness scope).
    ///
    /// The request lock must be set via set_request_lock() and must be
    /// recursive (see set_request_lock docs). When no lock is configured this
    /// is a no-op. Inner run_operation() calls within the guard re-acquire the
    /// lock on the same thread harmlessly (recursive re-entry); other threads
    /// block on the lock for the lifetime of the guard.
    ///
    ///     auto ex = nc.exclusive();   // lock held here
    ///     nc.execute(req1);           // inner lock re-acquire: no-op (recursive)
    ///     nc.execute(req2);           // still inside the exclusive group
    ///                                 // ex destructs → lock released
    ///
    /// Note: exclusive() = exclusion only. To hold RTX readiness across the
    /// group as well, combine with keep_ready() (the two are independent).
    [[nodiscard]] ExclusiveSession exclusive() {
        return ExclusiveSession{request_lock_};
    }

    /// Hold the RTX/CTX readiness scope across a group of requests so the
    /// card is asserted-ready once for the whole group instead of per request
    /// (readiness only — does NOT acquire the request lock).
    ///
    /// When NOTE_TXN_HANDSHAKE is enabled, the guard calls begin_operation()
    /// on the outermost entry and end_operation() on destruction. Inner
    /// run_operation() calls see op_depth_ > 0 and skip begin_operation —
    /// the card stays asserted-ready for all requests in the group.
    ///
    /// When NOTE_TXN_HANDSHAKE is off (the default for AVR/minimal builds)
    /// this returns a trivial empty guard with no op_depth_ touch.
    ///
    ///     auto kr = nc.keep_ready();  // begin_operation once (RTX asserted)
    ///     nc.execute(req1);           // op_depth_ > 0 → no inner begin_operation
    ///     nc.execute(req2);           // same
    ///                                 // kr destructs → end_operation (RTX released)
    ///
    /// Note: keep_ready() = readiness only. To atomically exclude other threads
    /// as well, combine with exclusive() (the two are independent).
    ///
    /// THREADING: ReadySession mutates op_depth_ (a plain int) outside the
    /// request lock. On a Notecard shared between threads you MUST hold
    /// exclusive() for the duration — declare it BEFORE keep_ready() so the
    /// lock is acquired first and released last:
    ///     auto ex = nc.exclusive();   // acquire the lock first
    ///     auto kr = nc.keep_ready();  // then open the readiness scope
    /// Using keep_ready() alone on a multi-threaded Notecard, or declaring it
    /// before exclusive(), is a data race on op_depth_. Single-threaded use is
    /// always safe.
    [[nodiscard]] ReadySession keep_ready() {
        return ReadySession{this};
    }

    /// Operation gate. Acquires the operation lock (if any) before doing
    /// anything else, so that cross-thread exclusion is immediate: a second
    /// thread entering any operation blocks here until the first thread's
    /// outermost operation completes.
    ///
    /// The lock must be recursive: same-thread nested calls (e.g. execute()
    /// called from within do_binary_send) re-acquire it on the same thread,
    /// which a recursive lock handles without deadlock. Within run_operation,
    /// op_depth_ is mutated only while the lock is held (or when no lock is
    /// configured), so it is race-free here. (The public keep_ready() guard
    /// also touches op_depth_ outside the lock — see its threading note: on a
    /// shared Notecard it must be paired with exclusive() held first.)
    ///
    /// When NOTE_TXN_HANDSHAKE is enabled, the outermost operation calls
    /// transport_->begin_operation() once at entry and transport_->end_operation()
    /// once at exit, so the RTX/CTX readiness gate is held for the whole
    /// operation (including multi-exchange ops like binary transfers) rather
    /// than toggled per wire exchange.
    ///
    /// If begin_operation() returns false (Notecard not ready within timeout),
    /// the operation proceeds anyway — the subsequent transact() will fail
    /// naturally with a transport-level error. Fast-fail on NotReady is a
    /// future improvement; correctness is maintained because the wire-level
    /// exchange will time out.
    template<typename Fn>
    auto run_operation(Fn&& fn) -> decltype(fn()) {
        if (request_lock_) request_lock_->lock();  // recursive: same thread re-enters, other threads block
        const bool outermost = (op_depth_++ == 0);
#if NOTE_TXN_HANDSHAKE
        if (outermost && transport_) transport_->begin_operation(default_timeout_ms_);
#endif
        struct Exit {
            Notecard* self;
            bool outermost;
            ~Exit() {
#if NOTE_TXN_HANDSHAKE
                if (outermost && self->transport_) self->transport_->end_operation();
#endif
                --self->op_depth_;
                if (self->request_lock_) self->request_lock_->unlock();
            }
        } exit_guard{this, outermost};
        return fn();
    }

private:
    using InplaceBuilder = void(*)(JsonRender& w, const void* ctx);

    /// Out-of-line workhorse for `transact_raw_inplace`. One copy shared
    /// across all lambda types — only the per-Fn trampoline above varies.
    Result<string_view> transact_raw_inplace_impl_(char* buf, size_t cap,
                                                    InplaceBuilder fn,
                                                    const void* ctx,
                                                    uint32_t timeout_ms) {
        if (!transport_)
            return make_error(Error::NotReady, NOTE_ERR("no transport configured"));
        JsonRender w(buf, cap);
        fn(w, ctx);
        w.close();
        if (w.overflow())
            return make_error(Error::Overflow, Cause::Unspecified,
                              NOTE_ERR("transact_raw_inplace: buffer overflow"));
        const size_t req_size = w.size();
        return transport_->transact(string_view(buf, req_size),
                                     span<char>(buf, cap), timeout_ms);
    }

    /// RAII guard that holds the bus lock across a multi-call raw byte sequence
    /// (e.g. the COBS payload stream in a binary transfer). Calls
    /// transport_->begin_bus_hold() on construction and end_bus_hold() on
    /// destruction. The bus lock is non-recursive — only one BusHoldGuard may
    /// be active at a time. No-op when NOTE_I2C_BUS_LOCK is disabled or when
    /// transport_ is null. BusHoldGuard must not be nested inside a transact/send
    /// call (which holds its own per-exchange BusLockGuard on Protocol).
    ///
    /// This is the Notecard-layer twin of Protocol's BusLockGuard: identical in
    /// shape but it locks through the ITransact::begin_bus_hold/end_bus_hold
    /// seam, because Notecard holds only an ITransact* and deliberately cannot
    /// reach Protocol's private IBusLock* (the transport abstracts away whether
    /// it even has a bus). Keeping them separate preserves that layer boundary.
#if NOTE_I2C_BUS_LOCK
    struct BusHoldGuard {
        ITransact* t_;
        explicit BusHoldGuard(ITransact* t) : t_(t) { if (t_) t_->begin_bus_hold(); }
        ~BusHoldGuard() { if (t_) t_->end_bus_hold(); }
        BusHoldGuard(const BusHoldGuard&) = delete;
        BusHoldGuard& operator=(const BusHoldGuard&) = delete;
        BusHoldGuard(BusHoldGuard&&) = delete;
        BusHoldGuard& operator=(BusHoldGuard&&) = delete;
    };
#endif // NOTE_I2C_BUS_LOCK

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
        // Hold the bus lock across the entire payload stream so no other
        // master can interleave between COBS blocks (releasing between blocks
        // risks the Notecard's intra-transaction receive timeout).
        // The guard is a no-op when NOTE_I2C_BUS_LOCK is disabled.
        bool tx_ok = true;
        {
#if NOTE_I2C_BUS_LOCK
            BusHoldGuard bus_hold{transport_};
#endif
            CobsEncoder encoder;
            encoder.encode(src.data(), src.size(), [&](const uint8_t* block, size_t n) {
                if (tx_ok) tx_ok = !!binary_write(block, n);
            });
            if (tx_ok) {
                uint8_t eop = cobs_eop;
                tx_ok = !!binary_write(&eop, 1);
            }
        } // BusHoldGuard released here — before the verify control sub-request
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

        // Hold the bus lock across the entire receive payload stream so no other
        // master can interleave between COBS chunk reads.
        // Released before MD5 verification (which does not use the raw bus).
        bool recv_ok = true;
        {
#if NOTE_I2C_BUS_LOCK
            BusHoldGuard bus_hold{transport_};
#endif
            uint8_t chunk[64];
            bool eop_seen = false;
            while (!eop_seen) {
                auto r = binary_read(chunk, sizeof(chunk), default_timeout_ms_);
                if (!r) {
                    recv_ok = false;
                    break;
                }
                size_t n = *r;
                for (size_t i = 0; i < n; ++i) {
                    if (chunk[i] == cobs_eop) { eop_seen = true; n = i + 1; break; }
                }
                decoder.feed(chunk, n, decode_sink);
            }
            if (recv_ok) decoder.flush(decode_sink);
        } // BusHoldGuard released here
        if (!recv_ok) {
            binary_io_reset();
            return ApiResult<Rsp>(
                ErrorInfo{Error::ResponseLost, Cause::Timeout, NOTE_ERR("binary receive timeout")});
        }

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

    /// Binary I/O: write bytes via the configured transport.
    Result<void> binary_write(const uint8_t* data, size_t len) {
        if (!transport_) return make_error(Error::NotReady, NOTE_ERR("no transport for binary I/O"));
        return transport_->write(data, len);
    }
    /// Binary I/O: read bytes via the configured transport.
    Result<size_t> binary_read(uint8_t* buf, size_t max_len, uint32_t timeout_ms) {
        if (!transport_) return make_error(Error::NotReady, NOTE_ERR("no transport for binary I/O"));
        return transport_->read(buf, max_len, timeout_ms);
    }
    void binary_io_reset() {
        if (transport_) transport_->reset();
    }

    /// Send a raw JSON control command for binary transfer and return
    /// the response as a string_view (into binary_ctrl_buf_).
    Result<string_view> binary_control(string_view json) {
        if (!transport_)
            return make_error(Error::NotReady, NOTE_ERR("no transport for binary control"));
        return transport_->transact(json,
                                    span<char>(binary_ctrl_buf_, sizeof(binary_ctrl_buf_)),
                                    default_timeout_ms_);
    }

    /// Extract an integer field from a raw JSON response (for binary control).
    static json_int_t binary_response_int(string_view json, string_view key) {
        // Minimal parse: find "key":NUMBER in the JSON
        JsonSink null_sink;
        struct IntCapture : JsonSink {
            string_view target_key;
            json_int_t value = 0;
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

    /// Streaming execute: SAX-stream the response directly into the
    /// typed `Rsp::Sink`. No tree, no intermediate buffer — the
    /// response struct is filled as bytes arrive on the wire.
    ///
    /// Sibling of `execute_tree`; the two embody the two response
    /// presentations (streaming vs tree). The caller (`execute`)
    /// picks based on whether the response type defines a Sink and
    /// whether an allocator is configured.
    ///
    /// Preconditions: alloc_.has_value() and (std::is_void_v<Rsp> ||
    /// detail::has_sink<Rsp>::value). Both are checked at the call
    /// site in `execute`.
    template<typename RequestT>
    ApiResult<typename RequestT::Response> execute_streaming(const RequestT& req) {
        using Rsp = typename RequestT::Response;

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
            if (ei.code != Error{}) return ApiResult<void>(ei);
            return ApiResult<void>{};
        } else {
            Rsp rsp{};
            auto reset_rsp = [](void* p) { *static_cast<Rsp*>(p) = Rsp{}; };
            alignas(body_sink_storage_align) char body_storage[body_sink_storage_size];
            BodyHandlerFactory body_factory = nullptr;
            void* body_ptr = nullptr;
            if constexpr (detail::has_body_factory<RequestT>::value) {
                body_factory = req.body_handler_factory_;
                body_ptr = req.body_ptr_;
            }
            auto ei = streaming_execute_typed<typename Rsp::Sink>(
                frame, rsp, RequestT::safety, reset_rsp, &rsp,
                body_factory, body_ptr, body_storage);
            if (ei.code != Error{}) return ApiResult<Rsp>(ei);
            // Capture the Allocator value on the Response so its
            // destructor can release interned strings when the caller
            // drops the result. Only fires on Responses that carry
            // string fields (codegen-gated).
            detail::attach_allocator(rsp, *alloc_);
            return ApiResult<Rsp>(std::move(rsp));
        }
    }

    /// Tree-mode execute: SAX-stream the response into the backend's
    /// tree-builder sink, then walk the assembled JsonReader.
    ///
    /// Separated from execute() so LTO can eliminate it when backend_
    /// is null. Renamed from execute_buffered — "tree" matches the
    /// docs' presentation vocabulary (streaming vs tree) and is more
    /// accurate after the SAX-events-in cutover: wire bytes no longer
    /// sit in a buffer between transport and decode; they stream
    /// directly into the backend's sink, which assembles the tree
    /// without an intermediate text materialisation (cJSON, nlohmann)
    /// or via SaxToTextSink → jsmn parse (StaticJsonBackend).
    ///
    /// This is what makes JSONB+tree compose: the transport hands SAX
    /// events to the backend regardless of wire format.
    template<typename RequestT>
    ApiResult<typename RequestT::Response> execute_tree(const RequestT& req, uint32_t req_id = 0) {
        using Rsp = typename RequestT::Response;

        // Build the request through the same RequestSource shape the
        // streaming path uses. The transport (JsonRequestTransport /
        // JsonbRequestTransport) picks the wire encoder; the backend's
        // outgoing text builder is no longer on the critical path.
        debug_timing(debug_, TimingEvent::BuildBegin, RequestT::notecard_request);
        auto build = [&](JsonBuilder& b) { req.build(b); };
        BuildFn build_fn = [](JsonBuilder& b, void* p) {
            (*static_cast<decltype(build)*>(p))(b);
        };
        RequestFrame frame{build_fn, &build, RequestT::notecard_request, req_id};
        BuildFnRequestSource src(framed_build, &frame);
        debug_timing(debug_, TimingEvent::BuildEnd, RequestT::notecard_request);

        // Debug-send: materialise the request bytes for the on_wire
        // hook. Same pattern as `streaming_attempt` — build the
        // request a second time into a scratch buffer so the user
        // sees the actual JSON. Only fires when debug.on_wire is set.
        if (debug_.on_wire) {
            char dbg_buf[NOTE_DBG_WIRE_BUF_SIZE];
            JsonBufferWriter dbg_w(dbg_buf, sizeof(dbg_buf));
            src.emit(dbg_w);
            dbg_w.write('}');
            debug_wire(debug_, dbg_w.view(), WireDirection::Send);
        }

        // Optional body-factory wiring. If the request has .into(T&),
        // the body handler intercepts top-level "body" events as they
        // arrive — same real-time approach the streaming path uses,
        // no post-hoc walk over the response.
        JsonSink& tree_sink = backend_->start_response(rsp_buf());

        // The body sink + pool + handler storage need to outlive the
        // transact() call below. optional<> rather than default-constructed
        // BodyDispatchSink so the sink only exists when a real handler does;
        // its on_*() paths can then assume `handler` is always set, which
        // collapses defensive `if (handler)` branches that are otherwise
        // unhittable (and inflate the coverage denominator).
        StringPool body_pool(alloc_value());
        alignas(body_sink_storage_align) char body_storage[body_sink_storage_size];
        std::optional<detail::BodyDispatchSink> body_sink;
        if constexpr (detail::has_body_factory<RequestT>::value) {
            if (req.body_handler_factory_) {
                auto bh = req.body_handler_factory_(req.body_ptr_, body_pool, body_storage);
                if (bh) body_sink.emplace(bh);
            }
        }

        // Debug-receive capture. When debug.on_wire is set, route SAX
        // events through a SaxToTextSink so we can log the response
        // text after the transact returns. Works uniformly for all
        // backends (direct-tree backends like cJSON don't preserve
        // text, so the dedicated capture is the only way to surface
        // the response wire bytes).
        detail::SaxToTextSink debug_recv;
        char debug_recv_buf[NOTE_DBG_WIRE_BUF_SIZE];
        const bool debug_active = (debug_.on_wire != nullptr);
        if (debug_active) {
            debug_recv.rearm(span<char>(debug_recv_buf, sizeof(debug_recv_buf)));
        }

        // Branch into the non-template dispatch helper. Keeping the
        // sink-chain selection out of the template body collapses lcov
        // branch counts from per-RequestT × per-combination back down
        // to one — and shrinks code size by an equivalent factor on
        // hosts that instantiate execute_tree across many RequestTs.
        Result<void> rv = transact_tree_(
            src.as_source(),
            tree_sink,
            body_sink    ? &*body_sink : nullptr,
            debug_active ? &debug_recv : nullptr,
            default_timeout_ms_);
        if (!rv) return Unexpected(rv.error());

        if (debug_active) {
            debug_wire(debug_, debug_recv.view(), WireDirection::Receive);
        }

        auto& reader = backend_->finish_response();
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
            // No allocator — transfer the response tree into an owning
            // reader that travels with the ApiResult. Keeps the err
            // string alive across subsequent transactions on this
            // Notecard (which would otherwise reset the backend's
            // in-flight reader).
            auto owned = backend_->release_response();
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
                    // Same RAII attach as the streaming path — once strings
                    // have been interned via *alloc_, the Response owns the
                    // cleanup until it's destroyed.
                    detail::attach_allocator(static_cast<Rsp&>(result), *alloc_);
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
        add_flash(b, flash(detail::common_keys::req), f.request_name);
        if (f.req_id) add_flash(b, flash(detail::common_keys::id),
                                static_cast<json_int_t>(f.req_id));
        f.inner(b, f.inner_ctx);
    }

    // ── Tree execute helpers (non-template) ─────────────────────────────

    /// Drive a tree-mode transact with optional body-handler and debug-receive
    /// fan-out sinks. Non-template so the four-way TeeSink chain compiles once,
    /// regardless of how many RequestT instantiations call execute_tree.
    Result<void> transact_tree_(RequestSource src, JsonSink& tree_sink,
                                JsonSink* body_sink, JsonSink* debug_recv,
                                uint32_t timeout_ms) {
        if (body_sink && debug_recv) {
            TeeSink t1{tree_sink, *body_sink};
            TeeSink t2{t1, *debug_recv};
            return transport_->transact(src, t2, timeout_ms);
        }
        if (body_sink) {
            TeeSink t{tree_sink, *body_sink};
            return transport_->transact(src, t, timeout_ms);
        }
        if (debug_recv) {
            TeeSink t{tree_sink, *debug_recv};
            return transport_->transact(src, t, timeout_ms);
        }
        return transport_->transact(src, tree_sink, timeout_ms);
    }

    // ── Streaming execute (non-template) ────────────────────────────────

    /// Single streaming attempt: transact + error capture.
    /// Pool is used for error message interning only.
    ErrorInfo streaming_attempt(RequestFrame& frame, JsonSink& sink) {
#if !NOTE_NO_STD_STRING
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
        BuildFnRequestSource src(framed_build, &frame);
        auto rv = transport_->transact(
            src.as_source(), err_sink, default_timeout_ms_);
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
            transport_->reset();
            sink.reset();
            if (reset_fn) reset_fn(reset_ctx);
        };
        auto result = retry_transaction<Result<void>>(
            hal(), timing_, safety, retry_policy_,
            attempt, reset);
        if (!result) return result.error();
        return {};
    }

    /// Typed streaming execute — thin template that constructs the
    /// per-type Sink, wires the (optional) body handler, and delegates to
    /// the non-template streaming_execute.
    ///
    /// body_factory is taken by value (function-pointer typedef) rather
    /// than as a template parameter so the branch shape doesn't multiply
    /// across per-RequestT lambda types (see `coverage` handoff).
    template<typename SinkT, typename RspT>
    ErrorInfo streaming_execute_typed(RequestFrame& frame, RspT& rsp,
                                      Safety safety,
                                      void (*reset_fn)(void*), void* reset_ctx,
                                      BodyHandlerFactory body_factory,
                                      void* body_ptr, void* body_storage) {
        StringPool pool(*alloc_);
        SinkT response_sink(rsp, pool);
        if constexpr (detail::has_set_body_handler<SinkT>::value) {
            if (body_factory) {
                BodyHandler bh = body_factory(body_ptr, pool, body_storage);
                if (bh) response_sink.set_body_handler(bh);
            }
        }
        JsonSinkAdapter<SinkT> virtual_sink(response_sink);
        return streaming_execute(frame, virtual_sink, safety, reset_fn, reset_ctx);
    }

    /// Enforce inter-transaction gap via the byte HAL.
    void enforce_timing() {
        if (!timing_.has_previous) return;
        if (!transport_) return;
        Hal& h = hal();
        uint32_t elapsed = h.millis() - timing_.last_transaction_end_ms;
        if (elapsed < timing_.min_gap_ms)
            h.delay(timing_.min_gap_ms - elapsed);
    }

    /// Record that a transaction just completed.
    void record_timing() {
        if (!transport_) return;
        timing_.last_transaction_end_ms = hal().millis();
        timing_.has_previous = true;
    }

    Allocator alloc_value() const { return alloc_.value_or(Allocator{}); }

    /// Copy `alloc_` (when present) into `note::detail::g_singleton_allocator`
    /// under `NOTE_SINGLETON=1`. Captured by value so the global survives
    /// Notecard moves / returns-by-value (factory patterns) without
    /// requiring a custom move ctor to chase the storage. No-op when
    /// SINGLETON is disabled. The slot has two consumers: the per-Response
    /// dtor under !NOTE_NO_RESPONSE_RAII, and the generated SINGLETON
    /// execute() which reads `g_singleton_allocator_present` to track
    /// the streaming-vs-tree path for body-having responses (latter is
    /// active under both RAII and NO_RAII builds).
    void publish_singleton_allocator_() {
#if NOTE_SINGLETON
        if (alloc_.has_value()) {
            ::note::detail::g_singleton_allocator = *alloc_;
            ::note::detail::g_singleton_allocator_present = true;
        } else {
            ::note::detail::g_singleton_allocator_present = false;
        }
#endif
    }

    IBusLock* request_lock_ = nullptr;  ///< Optional operation-level lock.
    int op_depth_ = 0;                  ///< Nesting depth for re-entrant guard.

    JsonBackend* backend_ = nullptr;
    ITransact* transport_ = nullptr;
    /// Set when the Notecard was constructed via a `Protocol&`-typed ctor.
    /// Used solely to drive the byte-by-byte growable response path in
    /// `transact(string_view) -> OwnedBuffer`, which needs the send/read
    /// split that only `Protocol` implements.
    Protocol* streaming_protocol_ = nullptr;
    uint32_t default_timeout_ms_ = 10000;
    std::optional<Allocator> alloc_;
    DebugListener debug_{};
    byte_span cobs_buf_{};          // optional external COBS working buffer
    /// Synchronous-response buffer for binary-transfer control commands
    /// (`{"req":"card.binary"}` status / `delete:true` reset). Fixed
    /// 256 bytes because:
    ///   - `do_binary_send`/`do_binary_receive` issue these queries
    ///     synchronously and need somewhere to land the response before
    ///     extracting `max`, `length`, `status` (MD5).
    ///   - Sized to fit the largest realistic card.binary status reply
    ///     (offset/length/cobs/max integers + a 32-byte MD5 + scaffolding).
    ///     Smaller responses leave headroom; the builder doesn't grow.
    ///   - Lives on the Notecard (not the request) so the binary-transfer
    ///     orchestration helpers can stay non-template and avoid pulling
    ///     in the full request type just for a status query.
    /// Trade-off: 256 bytes per Notecard even for builds that never use
    /// binary transfer. Acceptable on hosts/MCUs (Notecards are rarely
    /// instantiated more than once); not used on AVR-class targets.
    char binary_ctrl_buf_[256]{};
    /// Default response staging buffer. Used by `request()` /
    /// `execute_tree()` / `transact(json) -> OwnedBuffer` to copy the
    /// transport's response in before the JsonReader (or OwnedBuffer)
    /// walks it. Caller-supplied buffer (set via
    /// `set_response_buffer(span<char>)`) overrides this default.
    char rsp_buf_default_[NOTE_RSP_BUF_SIZE]{};
    span<char> rsp_buf_override_{};

    /// Return the active response staging buffer — caller-supplied
    /// override if `set_response_buffer()` was called, else the inline
    /// `rsp_buf_default_`. Computing the view on demand avoids storing a
    /// stale pointer through move-assignment of the Notecard
    /// (NotecardApi::begin moves a Notecard into nc_).
    span<char> rsp_buf() {
        if (!rsp_buf_override_.empty()) return rsp_buf_override_;
        return span<char>(rsp_buf_default_, NOTE_RSP_BUF_SIZE);
    }
    RetryPolicy retry_policy_{};
    TransactionTiming timing_{};
    uint32_t next_request_id_ = 1;
    bool request_ids_enabled_ = true;
#if !NOTE_NO_MD5
    // Static, shared across all Notecards: avoids a self-reference inside
    // Notecard (pointer to own member), which broke every move-assignment
    // done by e.g. NotecardApi::begin(transport). Both stock providers
    // (SoftwareMd5, MbedTlsMd5) are stateless, so sharing is safe.
    static inline PlatformMd5 default_md5_{};
    Md5Provider* md5_ = &default_md5_;
#else
    Md5Provider* md5_ = nullptr;
#endif

public:
    /// Access the current MD5 provider. Returns the default provider if
    /// none was explicitly installed via set_md5_provider. Exposed for
    /// introspection / tests; most callers never need this.
    Md5Provider* md5_provider() const { return md5_; }
};
#endif // NOTE_NO_POLYMORPHIC

} // namespace note
