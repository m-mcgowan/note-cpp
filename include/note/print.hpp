#pragma once
/// @file print.hpp
/// Arduino Print helpers for note-cpp types.
///
/// On Arduino, provides println() overloads that handle string_view and
/// ErrorInfo without manual .data()/.c_str() calls:
///
///     auto r = nc.card.version().execute();
///     if (r) {
///         note::println(Serial, r.version);
///     } else {
///         note::println(Serial, r.error());
///     }
///
/// On non-Arduino platforms, this header is empty.

#ifdef ARDUINO

#include <Arduino.h>
#include <string_view>

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

} // namespace note

#endif // ARDUINO
