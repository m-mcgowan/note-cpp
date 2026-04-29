#pragma once

/// @file request_source.hpp
/// Concrete `RequestSource` adapters for `ITransport`'s RequestSource
/// overloads. Each adapter exposes:
///   - `emit(JsonWriter&)` — duck-typed entry for templated dispatch.
///   - `as_source()`       — returns a `RequestSource` POD for the
///                           type-erased `ITransport` virtuals.
///
/// Builder-shape adapters:
///   - `BuilderRequestSource<F>` — templated on a callable `f(JsonBuilder&)`.
///     `emit()` instantiates a `StreamingJsonBuilder` (or
///     `StreamingJsonbBuilder` when `NOTE_JSONB=1`) over the writer and
///     runs the callable.
///   - `BuildFnRequestSource` — non-template adapter for the legacy
///     `BuildFn = void(*)(JsonBuilder&, void*)` shape. Used by Notecard
///     during the BuildFn → RequestSource migration (Phase 5a steps 4-7).
///
/// The closing `}` plus CRC suffix and line terminator are appended by
/// the protocol layer (`Protocol::stream_request_source`), not by the
/// source.
///
/// A verbatim string-shape adapter (sending pre-built JSON through
/// `RequestSource`) is deferred until Phase 5b's field router lands —
/// without it, the protocol would CRC-wrap and re-close a string that
/// already includes its own `}`. Until then, callers with pre-built
/// strings keep using the legacy `transact(string_view, …)` overloads on
/// `ITransport`, which preserve the existing no-CRC verbatim semantics.

#include <note/json.hpp>
#include <note/transport.hpp>
#include <note/types.hpp>

#if NOTE_JSONB
#include <note/jsonb.hpp>
#endif

namespace note {

/// Type-erased build function for streaming request construction. Called
/// with a `JsonBuilder&` (the streaming builder over the wire) plus a
/// caller-supplied context pointer. Defined here so both the BuildFn-shaped
/// entry points on `Protocol` (`protocol.hpp`) and the
/// `BuildFnRequestSource` adapter below can share one typedef.
using BuildFn = void(*)(JsonBuilder&, void*);

/// Builder-driven source: instantiates a `StreamingJsonBuilder` (or
/// `StreamingJsonbBuilder` for JSONB) over the supplied writer and runs
/// the user's callable against it. `F` is any callable accepting a
/// `JsonBuilder&` (lambda, free function, functor).
///
/// The builder writes the opening `{` on construction; the callable adds
/// fields. The closing `}` plus CRC suffix and line terminator are
/// appended by the protocol layer (`Protocol::stream_request_source`),
/// not by the source.
template<typename F>
class BuilderRequestSource {
public:
    explicit BuilderRequestSource(F& fn) : fn_(fn) {}

    void emit(JsonWriter& w) const {
#if NOTE_JSONB
        StreamingJsonbBuilder b(w);
#else
        StreamingJsonBuilder b(w);
#endif
        fn_(b);
    }

    RequestSource as_source() {
        return RequestSource{
            +[](JsonWriter& w, void* p) {
                static_cast<BuilderRequestSource*>(p)->emit(w);
            },
            this
        };
    }

private:
    F& fn_;
};

/// BuildFn-shape source: non-template adapter for the
/// `BuildFn = void(*)(JsonBuilder&, void*)` signature used by Protocol's
/// BuildFn-shaped entry points. Same emit semantics as
/// `BuilderRequestSource` but takes a function pointer + context instead
/// of a callable reference, so each call site shares one instantiation.
class BuildFnRequestSource {
public:
    BuildFnRequestSource(BuildFn fn, void* ctx) : fn_(fn), ctx_(ctx) {}

    void emit(JsonWriter& w) const {
#if NOTE_JSONB
        StreamingJsonbBuilder b(w);
#else
        StreamingJsonBuilder b(w);
#endif
        fn_(b, ctx_);
    }

    RequestSource as_source() {
        return RequestSource{
            +[](JsonWriter& w, void* p) {
                static_cast<BuildFnRequestSource*>(p)->emit(w);
            },
            this
        };
    }

private:
    BuildFn fn_;
    void* ctx_;
};

}  // namespace note
