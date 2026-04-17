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

/// The integer type used for JSON integer fields.
/// Default: int64_t (matches note-c's JINTEGER). Under NOTE_INT32_MATH,
/// narrowed to int32_t to save ~286 bytes flash on AVR (software 64-bit
/// math elimination). int32_t overflows UNIX timestamps after 2038-01-19.
#if NOTE_INT32_MATH
using json_int_t = int32_t;
#else
using json_int_t = int64_t;
#endif

/// Data flow direction for binary operations.
enum class Direction { Send, Receive };

class JsonReader;  // Forward declaration — ApiResult holds reader to extend lifetime.

using string_view = std::string_view;

/// A string_view subtype for response array elements.
/// Adds c_str() (null-terminated guarantee from StringPool::intern())
/// and printTo() for Arduino.
struct printable_string_view : string_view {
    using string_view::string_view;
    using string_view::operator=;
    printable_string_view() = default;
    printable_string_view(string_view sv) : string_view(sv) {}

    /// Null-terminated C string access.
    const char* c_str() const { return data(); }

#ifdef ARDUINO
    size_t printTo(Print& p) const {
        return p.write(reinterpret_cast<const uint8_t*>(data()), size());
    }
#endif
};

namespace detail {
#if defined(__cpp_lib_expected) && __cpp_lib_expected >= 202202L
    template<typename T, typename E> using expected = std::expected<T, E>;
    template<typename E> using unexpected = std::unexpected<E>;
#else
    template<typename T, typename E> using expected = tl::expected<T, E>;
    template<typename E> using unexpected = tl::unexpected<E>;
#endif
} // namespace detail

/// Result type — extends expected with a printTo() method for Arduino.
///
/// Does NOT inherit Printable to avoid adding a vtable pointer to every
/// Result (used in every API call). Call printTo() directly:
///
///   auto r = nc.transact(json);
///   r.printTo(Serial);  // prints response or "Error: ..."
///
/// For contexts that require Printable (e.g. Serial.println()), use
/// PrintableResult(r) wrapper.
template<typename T>
class Result : public detail::expected<T, ErrorInfo> {
    using Base = detail::expected<T, ErrorInfo>;
public:
    using Base::Base;
    Result(const Base& b) : Base(b) {}
    Result(Base&& b) : Base(std::move(b)) {}

#ifdef ARDUINO
    /// Print the value (if Printable) or error message.
    /// Non-virtual — does not add vtable overhead.
    size_t printTo(Print& p) const {
        if (this->has_value()) {
            if constexpr (std::is_base_of_v<Printable, std::decay_t<T>>)
                return this->value().printTo(p);
            else
                return p.print("(ok)");
        } else {
            size_t n = p.print("Error: ");
            auto& e = this->error();
            n += p.write(reinterpret_cast<const uint8_t*>(e.message.data()), e.message.size());
            return n;
        }
    }
#endif
};

#ifdef ARDUINO

#if __cplusplus >= 202002L
/// Concept: type has a printTo(Print&) const method.
template<typename T>
concept HasPrintTo = requires(const T& v, Print& p) {
    { v.printTo(p) } -> std::convertible_to<size_t>;
};
#endif

/// Thin Printable wrapper for any type with a printTo(Print&) method.
/// Zero-copy: holds a reference to the original object.
///   Serial.println(printable(r));
#if __cplusplus >= 202002L
template<HasPrintTo T>
#else
template<typename T>
#endif
class PrintableWrapper : public Printable {
    const T& ref_;
public:
    explicit PrintableWrapper(const T& r) : ref_(r) {}
    size_t printTo(Print& p) const override { return ref_.printTo(p); }
};

#if __cplusplus >= 202002L
template<HasPrintTo T>
#else
template<typename T>
#endif
PrintableWrapper<T> printable(const T& v) { return PrintableWrapper<T>(v); }

template<typename T>
using PrintableResult = PrintableWrapper<Result<T>>;

#endif // ARDUINO

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
#if !NOTE_NO_BUFFERED
    std::unique_ptr<JsonReader> reader_;  // keeps error message string_views alive
#endif
public:
    ApiResult(Response r) : Response(std::move(r)) {}
    ApiResult(ErrorInfo e) : err_(std::move(e)) {}
#if !NOTE_NO_BUFFERED
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

} // namespace note
