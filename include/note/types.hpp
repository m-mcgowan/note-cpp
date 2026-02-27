#pragma once

#include <cstdint>
#include <expected>    // C++23: std::expected
#include <functional>
#include <memory>
#include <optional>
#include <string_view>

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

using string_view = std::string_view;

// C++23: std::expected — retrofit to tl::expected for C++17
template<typename T>
using Result = std::expected<T, ErrorInfo>;

// C++23: std::unexpected — retrofit to tl::unexpected for C++17
using Unexpected = std::unexpected<ErrorInfo>;

inline Unexpected make_error(Error code, string_view message = {}) {
    if (message.empty()) message = to_string(code);
    return Unexpected(ErrorInfo{code, message});
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
public:
    ApiResult(Response r) : Response(std::move(r)) {}
    ApiResult(ErrorInfo e) : err_(std::move(e)) {}
    ApiResult(Unexpected e) : err_(std::move(e).error()) {}

    explicit operator bool() const { return !err_.has_value(); }
    bool has_value() const { return !err_.has_value(); }

    const ErrorInfo& error() const { return *err_; }
};

} // namespace note
