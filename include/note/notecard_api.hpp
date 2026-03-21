#pragma once
/// @file notecard_api.hpp
/// NotecardApi — convenience wrapper combining Notecard and Api for the
/// common case of a single Notecard on one transport.
///
/// Instead of constructing a Notecard and an Api separately:
///
///     note::Notecard nc(backend, transport);
///     note::Api api(nc);
///     api.hub.set().product("com.example.app").execute();
///
/// Use NotecardApi for a simpler setup:
///
///     note::NotecardApi nc(backend, transport);
///     nc.hub.set().product("com.example.app").execute();

#include "api.hpp"
#include "notecard.hpp"

namespace note {

namespace detail {
    // Base class to ensure Notecard is constructed before Api.
    struct NcHolder {
        Notecard owned_nc_;

        NcHolder(JsonBackend& backend, Notecard::RequestFn request_fn,
                 Notecard::SendFn send_fn = {})
            : owned_nc_(backend, std::move(request_fn), std::move(send_fn)) {}
    };
} // namespace detail

/// Single-object entry point for Notecard communication.
///
/// Owns a Notecard and exposes the full typed Api surface directly.
/// For the 99% case where you have one Notecard on one transport.
///
/// @code
/// note::NotecardApi nc(backend, transport);
/// nc.hub.set().product("com.example.app").mode("periodic").execute();
/// auto r = nc.card.version().execute();
/// @endcode
#if __cplusplus >= 202002L
template<typename TargetT = Unconstrained>
class NotecardApi : private detail::NcHolder, public Api<TargetT> {
public:
    NotecardApi(JsonBackend& backend, Notecard::RequestFn request_fn,
                Notecard::SendFn send_fn = {})
        : detail::NcHolder(backend, std::move(request_fn), std::move(send_fn))
        , Api<TargetT>(owned_nc_) {}

    NotecardApi(JsonBackend& backend, Notecard::RequestFn request_fn,
                TargetT target, Notecard::SendFn send_fn = {})
        : detail::NcHolder(backend, std::move(request_fn), std::move(send_fn))
        , Api<TargetT>(owned_nc_, target) {}

    /// Access the underlying Notecard for transport-level operations.
    Notecard& notecard() { return owned_nc_; }
};
#else
class NotecardApi : private detail::NcHolder, public Api {
public:
    NotecardApi(JsonBackend& backend, Notecard::RequestFn request_fn,
                Notecard::SendFn send_fn = {})
        : detail::NcHolder(backend, std::move(request_fn), std::move(send_fn))
        , Api(owned_nc_) {}

    Notecard& notecard() { return owned_nc_; }
};
#endif

} // namespace note
