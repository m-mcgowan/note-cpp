#pragma once
/// @file print.hpp
/// Arduino Print helpers for note-cpp types.
///
/// On non-Arduino platforms, this header is empty.

#ifdef ARDUINO

#include <string_view>
#include <type_traits>

namespace note {

/// Print a string_view to an Arduino Print stream (Serial, etc.).
inline size_t println(Print& p, std::string_view sv) {
    size_t n = p.write(reinterpret_cast<const uint8_t*>(sv.data()), sv.size());
    n += p.println();
    return n;
}

/// Print a string_view without trailing newline.
inline size_t print(Print& p, std::string_view sv) {
    return p.write(reinterpret_cast<const uint8_t*>(sv.data()), sv.size());
}

namespace detail {

/// Compile-time JSON value printer for Arduino printTo() methods.
/// Dispatches based on the value type — handles string_view, bool,
/// integral types (including duration wrappers like Seconds/Minutes
/// that convert to int32_t), and floating point.
template<typename T>
inline size_t print_json_value(Print& p, const T& v) {
    if constexpr (std::is_same_v<T, bool>) {
        return p.print(v ? "true" : "false");
    } else if constexpr (std::is_same_v<T, std::string_view>) {
        size_t n = p.print("\"");
        n += p.write(reinterpret_cast<const uint8_t*>(v.data()), v.size());
        n += p.print("\"");
        return n;
    } else if constexpr (std::is_floating_point_v<T>) {
        return p.print(static_cast<double>(v));
    } else if constexpr (std::is_convertible_v<T, long>) {
        // Covers int32_t, Seconds, Minutes, Hours, Days, and any
        // other type with an implicit conversion to an integer.
        return p.print(static_cast<long>(v));
    } else {
        // Fallback: treat as a string-like type with .data()
        size_t n = p.print("\"");
        n += p.print(v.data());
        n += p.print("\"");
        return n;
    }
}

} // namespace detail

} // namespace note

#endif // ARDUINO
