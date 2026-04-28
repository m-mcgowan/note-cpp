#pragma once

/// @file request_source.hpp
/// Concrete `RequestSource` adapters for `ITransport`'s RequestSource
/// overloads. Each adapter exposes:
///   - `emit(JsonWriter&)` — duck-typed entry for templated dispatch.
///   - `as_source()`       — returns a `RequestSource` POD for the
///                           type-erased `ITransport` virtuals.
///
/// Phase 5a step 3 ships only the builder-shape adapter:
///   - `BuilderRequestSource<F>` — runs a callable `f(JsonBuilder&)` against
///     a `StreamingJsonBuilder` (or `StreamingJsonbBuilder` when
///     `NOTE_JSONB=1`) layered over the writer. The closing `}` plus CRC
///     suffix and line terminator are appended by the protocol layer.
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

}  // namespace note
