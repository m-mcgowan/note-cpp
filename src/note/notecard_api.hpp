#pragma once

// NotecardApi requires the polymorphic Notecard class, which is not
// available under NOTE_NO_POLYMORPHIC (use StaticNotecard + Api<NcT> instead).
#if NOTE_NO_POLYMORPHIC
// Empty — NotecardApi not available under NOTE_NO_POLYMORPHIC.
#else

/// @file notecard_api.hpp
/// NotecardApi — single-object entry point for Notecard communication.
///
/// Simplest setup (with default backend):
///
///     note::NotecardApi nc;
///     nc.begin(transport);
///     nc.hub.set().product("com.example.app").execute();
///
/// Or with an explicit backend:
///
///     note::NotecardApi nc(myBackend, transport);
///     nc.hub.set().product("com.example.app").execute();

#include "allocator.hpp"
#include "api.hpp"
#include "notecard.hpp"
#include "backends/buffer.hpp"

namespace note {

namespace detail {

    /// Default JSON backend: zero-heap StaticJsonBackend.
    /// 512-byte build buffer + 64 jsmn tokens covers typical Notecard requests.
    using DefaultBackend = backends::StaticJsonBackend<512, 64>;

    /// Owns the backend + Notecard, ensuring construction order.
    struct NcOwner {
        DefaultBackend default_backend_;
        Notecard nc_;

        NcOwner()
            : nc_() {}

        explicit NcOwner(ITransact& transport)
            : nc_(default_backend_, transport) {}

        NcOwner(JsonBackend& backend, ITransact& transport)
            : nc_(backend, transport) {}
    };

} // namespace detail


/// Single-object entry point for Notecard communication.
///
/// Owns a JSON backend and Notecard, and exposes the full typed Api surface.
/// For the 99% case where you have one Notecard on one transport.
///
/// @code
/// // Default backend, transport set later:
/// note::NotecardApi nc;
/// nc.begin(transport);
///
/// // Or all at once:
/// note::NotecardApi nc(transport);
///
/// // Then use the typed API directly:
/// nc.hub.set().product("com.example.app").mode("periodic").execute();
///
/// // Binary transfer:
/// nc.card.binary.put().data(buf, len).execute();
/// nc.card.binary.get().into(dst, sizeof(dst)).length(N).execute();
/// @endcode
#if __cplusplus >= 202002L
template<typename TargetT = Unconstrained>
class NotecardApi : private detail::NcOwner, public Api<TargetT> {
public:
    /// Default constructor — call begin() before making requests.
    NotecardApi()
        : detail::NcOwner()
        , Api<TargetT>(nc_) {}

    /// Variadic axis pack — constrains TargetT via CTAD (see deduction guide
    /// below). Axes are compile-time tags only; runtime state matches the
    /// default constructor.
    ///
    ///   note::NotecardApi nc(note::sku::NOTE_ESP, note::fw::v7_5_1);
    template<typename... Axes>
        requires (sizeof...(Axes) > 0
                  && (detail::HasAxisCategory<Axes> && ...))
    NotecardApi(Axes...)
        : detail::NcOwner()
        , Api<TargetT>(nc_) {}

    /// Construct with transport (uses default backend).
    explicit NotecardApi(ITransact& transport)
        : detail::NcOwner(transport)
        , Api<TargetT>(nc_) {}

    /// Construct with explicit backend + transport.
    NotecardApi(JsonBackend& backend, ITransact& transport)
        : detail::NcOwner(backend, transport)
        , Api<TargetT>(nc_) {}

    /// Set a Protocol-typed transport with explicit allocator (streaming-only,
    /// growable OwnedBuffer responses; no JsonBackend needed).
    void begin(Protocol& transport, Allocator alloc) {
        nc_ = Notecard(transport, alloc);
    }

    /// Set a Protocol-typed transport with the default heap allocator.
    void begin(Protocol& transport) {
        nc_ = Notecard(transport, Allocator{});
    }

    /// Set the transport after construction (uses default backend, lights
    /// up tree-mode `body()` walking).
    void begin(ITransact& transport) {
        nc_ = Notecard(default_backend_, transport);
    }

    /// Set the transport with an explicit JsonBackend.
    void begin(JsonBackend& backend, ITransact& transport) {
        nc_ = Notecard(backend, transport);
    }

    /// Validated JSON passthrough — caller-supplied buffer. Sends the
    /// pre-formatted request, copies the response into `buf`, and
    /// returns a `string_view` into `buf`. Works on both streaming and
    /// buffered transports. Use this when you need the raw response
    /// (e.g. to walk it with `note::scan`) instead of going through the
    /// typed builders.
    Result<string_view> transact(string_view json, span<char> buf) {
        return nc_.transact(json, buf);
    }

    /// Validated JSON fire-and-forget — pre-formatted request, no
    /// response. Forwarded to Notecard::send.
    Result<void> send(string_view json) {
        return nc_.send(json);
    }

    /// One-shot `echo` connectivity probe. Forwarded to Notecard::ping;
    /// see the description there for the wire shape, timing, and the
    /// meaning of `seed_fn`.
    Result<void> ping(uint32_t timeout_ms = 500, PingSeedFn seed_fn = nullptr) {
        return nc_.ping(timeout_ms, seed_fn);
    }

    Notecard& notecard() { return nc_; }
};

/// CTAD guide: map a pack of axis values at the call site to
/// `NotecardApi<ComposedTarget<Axes...>>`. The requires clause keeps
/// non-axis arguments (e.g. `ITransact&`) from matching this guide so
/// their constructors deduce the default `TargetT=Unconstrained`.
template<typename... Axes>
    requires (sizeof...(Axes) > 0
              && (detail::HasAxisCategory<Axes> && ...))
NotecardApi(Axes...) -> NotecardApi<ComposedTarget<Axes...>>;

#else
class NotecardApi : private detail::NcOwner, public Api<Notecard> {
    // Resolve ambiguous nc_ (NcOwner::nc_ vs Api::nc_).
    Notecard& nc() { return detail::NcOwner::nc_; }

public:
    NotecardApi()
        : detail::NcOwner()
        , Api<Notecard>(detail::NcOwner::nc_) {}

    explicit NotecardApi(ITransact& transport)
        : detail::NcOwner(transport)
        , Api<Notecard>(detail::NcOwner::nc_) {}

    NotecardApi(JsonBackend& backend, ITransact& transport)
        : detail::NcOwner(backend, transport)
        , Api<Notecard>(detail::NcOwner::nc_) {}

    void begin(Protocol& transport, Allocator alloc) {
        detail::NcOwner::nc_ = Notecard(transport, alloc);
    }

    void begin(Protocol& transport) {
        detail::NcOwner::nc_ = Notecard(transport, Allocator{});
    }

    void begin(ITransact& transport) {
        detail::NcOwner::nc_ = Notecard(default_backend_, transport);
    }

    void begin(JsonBackend& backend, ITransact& transport) {
        detail::NcOwner::nc_ = Notecard(backend, transport);
    }

    /// Validated JSON passthrough — see C++20 overload above.
    Result<string_view> transact(string_view json, span<char> buf) {
        return nc().transact(json, buf);
    }

    /// Validated JSON fire-and-forget — see C++20 overload above.
    Result<void> send(string_view json) {
        return nc().send(json);
    }

    /// One-shot `echo` connectivity probe — see C++20 overload above.
    Result<void> ping(uint32_t timeout_ms = 500, PingSeedFn seed_fn = nullptr) {
        return nc().ping(timeout_ms, seed_fn);
    }

    Notecard& notecard() { return nc(); }
};
#endif

} // namespace note

#endif // NOTE_NO_POLYMORPHIC
