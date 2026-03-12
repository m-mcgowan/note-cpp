#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>

// C++23 std::expected — use the standard library when available, otherwise
// fall back to tl::expected (a single-header backport, CC0 public domain).
#if __has_include(<version>)
#   include <version>
#endif

#if defined(__cpp_lib_expected) && __cpp_lib_expected >= 202202L
#   include <expected>
#else
#   include "third_party/expected.hpp"
#endif

#include "error.hpp"

// Notecard API version gating.
//
// Define NOTE_API_VERSION before including any note headers to restrict
// the generated API to fields available on that firmware version.
// Defaults to the latest version if not defined.
//
// Version gating modes:
//   Default (NOTE_API_VERSION not defined): all fields available.
//   Warn (NOTE_API_VERSION defined): fields newer than your target produce
//     a [[deprecated]] compiler warning but remain visible in IDE autocomplete.
//   Strict (NOTE_API_STRICT also defined): newer fields are compiled out
//     entirely via #if guards.
#define NOTE_VERSION(major, minor, patch) \
    ((major) * 10000 + (minor) * 100 + (patch))

#ifndef NOTE_API_VERSION
#define NOTE_API_VERSION NOTE_VERSION(9, 1, 1)
#endif

namespace note {

class JsonReader;  // Forward declaration — ApiResult holds reader to extend lifetime.

using string_view = std::string_view;

namespace detail {
#if defined(__cpp_lib_expected) && __cpp_lib_expected >= 202202L
    template<typename T, typename E> using expected = std::expected<T, E>;
    template<typename E> using unexpected = std::unexpected<E>;
#else
    template<typename T, typename E> using expected = tl::expected<T, E>;
    template<typename E> using unexpected = tl::unexpected<E>;
#endif
} // namespace detail

template<typename T>
using Result = detail::expected<T, ErrorInfo>;

using Unexpected = detail::unexpected<ErrorInfo>;

inline Unexpected make_error(Error code, string_view message = {}) {
    if (message.empty()) message = to_string(code);
    return Unexpected(ErrorInfo{code, Cause::Unspecified, message});
}

inline Unexpected make_error(Error code, Cause cause, string_view message = {}) {
    if (message.empty()) message = to_string(code);
    return Unexpected(ErrorInfo{code, cause, message});
}

// Result type for typed API responses. Inherits from Response so fields
// are accessible with dot notation instead of arrow:
//
//   auto r = api.cardVersion().execute();
//   if (r) {
//       auto ver = r.version;   // dot, not r->version
//   }
//
// On error, response fields are default-initialized (zero/empty).
template<typename Response>
class ApiResult : public Response {
    std::optional<ErrorInfo> err_;
    std::unique_ptr<JsonReader> reader_;  // keeps error message string_views alive
public:
    ApiResult(Response r) : Response(std::move(r)) {}
    ApiResult(ErrorInfo e) : err_(std::move(e)) {}
    ApiResult(ErrorInfo e, std::unique_ptr<JsonReader> reader)
        : err_(std::move(e)), reader_(std::move(reader)) {}
#if defined(__cpp_lib_expected) && __cpp_lib_expected >= 202202L
    ApiResult(Unexpected e) : err_(std::move(e).error()) {}
#else
    ApiResult(Unexpected e) : err_(std::move(e).value()) {}
#endif

    explicit operator bool() const { return !err_.has_value(); }
    bool has_value() const { return !err_.has_value(); }

    const ErrorInfo& error() const { return *err_; }
};

} // namespace note
