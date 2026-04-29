#pragma once
/// @file notecard_api_fixture.hpp
/// Global Api accessor for shared integration tests.
///
/// Each environment (serial, I2C, softcard) defines g_api in its main.cpp
/// during PTR_BOARD_INIT. Shared test cases call notecard_api() to get it.

#include <note/api.hpp>
#include <note/error.hpp>
#include <note/protocol.hpp>
#include <doctest.h>

// Global Api instance — set by each environment's board init.
extern note::Api<>* g_api;

/// Streaming transport over the active interface (serial or I2C). Set by
/// each environment's board init alongside g_api. Tests that want to
/// exercise the same physical transport with a *buffered* (tree-mode)
/// Notecard pass this directly to
/// `note::Notecard(JsonBackend&, ITransact&)` — `Protocol`
/// satisfies `ITransact` natively, so the ctor lights up the buffered
/// execute path that supports `body()` walking.
extern note::Protocol* g_streaming_transport;

/// Get the global Api reference. Asserts that it was initialized.
inline note::Api<>& notecard_api() { return *g_api; }

/// Get the underlying Notecard (for passthrough, debug, etc.).
inline note::Notecard& notecard_nc() { return g_api->notecard(); }

// ── doctest stringification for ApiResult / Result ─────────────────────
// Lets REQUIRE(rsp) print the error details instead of {?} on failure.

namespace doctest {

template<typename T>
struct StringMaker<note::ApiResult<T>> {
    static String convert(const note::ApiResult<T>& r) {
        if (r) return "ApiResult{ok}";
        auto s = note::to_string(r.error());
        return String(s.data(), static_cast<unsigned>(s.size()));
    }
};

template<>
struct StringMaker<note::ApiResult<void>> {
    static String convert(const note::ApiResult<void>& r) {
        if (r) return "ApiResult<void>{ok}";
        auto s = note::to_string(r.error());
        return String(s.data(), static_cast<unsigned>(s.size()));
    }
};

template<typename T>
struct StringMaker<note::Result<T>> {
    static String convert(const note::Result<T>& r) {
        if (r) return "Result{ok}";
        auto s = note::to_string(r.error());
        return String(s.data(), static_cast<unsigned>(s.size()));
    }
};

} // namespace doctest
