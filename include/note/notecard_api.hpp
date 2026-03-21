#pragma once
/// @file notecard_api.hpp
/// NotecardApi — single-object entry point for Notecard communication.
///
/// Simplest setup (with default backend):
///
///     note::NotecardApi nc;
///     nc.begin(transport);  // transport is a callable or NotecardSerial/I2c
///     nc.hub.set().product("com.example.app").execute();
///
/// Or with an explicit backend:
///
///     note::NotecardApi nc(myBackend, transport);
///     nc.hub.set().product("com.example.app").execute();

#include "api.hpp"
#include "notecard.hpp"
#include "backends/buffer.hpp"

namespace note {

namespace detail {

    /// Default JSON backend: zero-heap BufferJsonBackend.
    /// 512-byte build buffer + 64 jsmn tokens covers typical Notecard requests.
    using DefaultBackend = backends::BufferJsonBackend<512, 64>;

    /// Stub transport that returns NotReady — used before begin() is called.
    inline Result<string_view> not_ready_fn(string_view, uint32_t) {
        return Unexpected(ErrorInfo{Error::NotReady, "call begin() first"});
    }

    /// Owns the backend + Notecard, ensuring construction order.
    struct NcOwner {
        DefaultBackend default_backend_;
        Notecard nc_;

        NcOwner()
            : nc_(default_backend_, not_ready_fn) {}

        NcOwner(Notecard::RequestFn request_fn, Notecard::SendFn send_fn = {})
            : nc_(default_backend_, std::move(request_fn), std::move(send_fn)) {}

        NcOwner(JsonBackend& backend, Notecard::RequestFn request_fn,
                Notecard::SendFn send_fn = {})
            : nc_(backend, std::move(request_fn), std::move(send_fn)) {}
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
/// @endcode
#if __cplusplus >= 202002L
template<typename TargetT = Unconstrained>
class NotecardApi : private detail::NcOwner, public Api<TargetT> {
public:
    /// Default constructor — call begin() before making requests.
    NotecardApi()
        : detail::NcOwner()
        , Api<TargetT>(nc_) {}

    /// Construct with transport callable (uses default backend).
    explicit NotecardApi(Notecard::RequestFn request_fn,
                         Notecard::SendFn send_fn = {})
        : detail::NcOwner(std::move(request_fn), std::move(send_fn))
        , Api<TargetT>(nc_) {}

    /// Construct with explicit backend + transport.
    NotecardApi(JsonBackend& backend, Notecard::RequestFn request_fn,
                Notecard::SendFn send_fn = {})
        : detail::NcOwner(backend, std::move(request_fn), std::move(send_fn))
        , Api<TargetT>(nc_) {}

    /// Set the transport after construction.
    /// On Arduino, typically called from setup():
    ///   nc.begin(transport);
    void begin(Notecard::RequestFn request_fn, Notecard::SendFn send_fn = {}) {
        nc_ = Notecard(default_backend_, std::move(request_fn), std::move(send_fn));
    }

    Notecard& notecard() { return nc_; }
};
#else
class NotecardApi : private detail::NcOwner, public Api {
public:
    NotecardApi()
        : detail::NcOwner()
        , Api(nc_) {}

    explicit NotecardApi(Notecard::RequestFn request_fn,
                         Notecard::SendFn send_fn = {})
        : detail::NcOwner(std::move(request_fn), std::move(send_fn))
        , Api(nc_) {}

    NotecardApi(JsonBackend& backend, Notecard::RequestFn request_fn,
                Notecard::SendFn send_fn = {})
        : detail::NcOwner(backend, std::move(request_fn), std::move(send_fn))
        , Api(nc_) {}

    void begin(Notecard::RequestFn request_fn, Notecard::SendFn send_fn = {}) {
        nc_ = Notecard(default_backend_, std::move(request_fn), std::move(send_fn));
    }

    Notecard& notecard() { return nc_; }
};
#endif

} // namespace note
