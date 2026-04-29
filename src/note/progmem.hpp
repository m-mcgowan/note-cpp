#pragma once

/// @file progmem.hpp
/// Flash string helpers for Harvard architecture platforms (AVR).
///
/// On AVR, string literals in .data consume RAM. These helpers let
/// generated code declare field name strings in flash (PROGMEM) and
/// access them without macros. On non-Harvard platforms, all functions
/// are trivial pass-throughs that the compiler optimizes away.

#include <cstddef>
#include <cstring>
#include <string_view>

#ifndef NOTE_PROGMEM
#  ifdef __AVR__
#    define NOTE_PROGMEM 1
#  else
#    define NOTE_PROGMEM 0
#  endif
#endif

#if NOTE_PROGMEM
#include <avr/pgmspace.h>
#endif

#ifdef ARDUINO
// Forward declaration: Arduino.h/WString.h declares __FlashStringHelper as
// an incomplete type. We forward-declare here so progmem.hpp is self-contained
// and compiles correctly before Arduino.h has been included.
class __FlashStringHelper;
#endif

namespace note {

/// A string stored in program memory (flash). On non-Harvard platforms,
/// this is just a regular string pointer — zero overhead.
struct FlashString {
    const char* ptr = nullptr;
    size_t len = 0;

    constexpr FlashString() = default;
    constexpr FlashString(const char* p, size_t l) : ptr(p), len(l) {}

#ifdef ARDUINO
    /// Implicit conversion from an Arduino `F("...")` expression.
    /// Lets scan/JsonView flash-key overloads accept `F()` directly
    /// without an explicit `note::flash()` wrapper.
    ///
    /// Marked always_inline: without it GCC emits the ctor out-of-line,
    /// costing ~100 B of flash across a handful of call sites.
    __attribute__((always_inline)) inline
    FlashString(const __FlashStringHelper* f)
        : ptr(reinterpret_cast<const char*>(f)),
#if NOTE_PROGMEM
          len(strlen_P(reinterpret_cast<const char*>(f)))
#else
          len(strlen(reinterpret_cast<const char*>(f)))
#endif
    {}
#endif

    /// Length in bytes (aliases `len` for std::string_view-like interface).
    constexpr size_t size() const { return len; }

    /// Byte access. On AVR with PROGMEM storage, issues an LPM read;
    /// elsewhere a plain load. No bounds check — caller checks `size()`.
    char operator[](size_t i) const {
#if NOTE_PROGMEM
        return static_cast<char>(pgm_read_byte(ptr + i));
#else
        return ptr[i];
#endif
    }

    /// Compare against a RAM std::string_view.
    bool operator==(std::string_view sv) const {
#if NOTE_PROGMEM
        return sv.size() == len && memcmp_P(sv.data(), ptr, len) == 0;
#else
        return sv == std::string_view(ptr, len);
#endif
    }

    /// Copy to a RAM buffer and return a std::string_view.
    /// buf must be at least len bytes.
    std::string_view to_view(char* buf) const {
#if NOTE_PROGMEM
        memcpy_P(buf, ptr, len);
#else
        memcpy(buf, ptr, len);
#endif
        return {buf, len};
    }

    /// On non-Harvard, return a std::string_view directly (no copy needed).
    /// On Harvard, this is UB — use to_view() instead.
#if !NOTE_PROGMEM
    std::string_view view() const { return {ptr, len}; }
#endif
};

/// Create a FlashString from a string literal declared with NOTE_FLASH.
/// On AVR, the literal must be in PROGMEM.
template<size_t N>
constexpr FlashString flash(const char (&s)[N]) {
    return {s, N - 1};
}

#ifdef ARDUINO
/// Build a FlashString from an Arduino `F("...")` expression. `F()`
/// always forces the string into PROGMEM, so this path is safe by
/// construction (no PROGMEM-declaration footgun).
inline FlashString flash(const __FlashStringHelper* f) {
    const char* p = reinterpret_cast<const char*>(f);
#if NOTE_PROGMEM
    return {p, strlen_P(p)};
#else
    return {p, strlen(p)};
#endif
}
#endif

/// Write a flash key + value to a JsonBuilder.
/// Copies the key to the stack on Harvard architectures.
template<typename Builder, typename T>
void add_flash(Builder& b, FlashString key, const T& value) {
#if NOTE_PROGMEM
    char buf[64]; // field names are short
    b.add(key.to_view(buf), value);
#else
    b.add(key.view(), value);
#endif
}

template<typename Builder>
void add_raw_flash(Builder& b, FlashString key, std::string_view value) {
#if NOTE_PROGMEM
    char buf[64];
    b.add_raw(key.to_view(buf), value);
#else
    b.add_raw(key.view(), value);
#endif
}

/// Attribute for declaring a string in program memory.
/// Usage: static const char name[] NOTE_FLASH_ATTR = "value";
#if NOTE_PROGMEM
#  define NOTE_FLASH_ATTR PROGMEM
#else
#  define NOTE_FLASH_ATTR
#endif

namespace detail {

/// Common PROGMEM-resident wire keys used outside generated endpoint
/// headers — the request/command framing layer (`req`/`cmd`/`id`) and
/// the GenericResponseSink body-depth gate (`body`). Centralising them
/// here keeps these short literals in flash on AVR (Harvard arch
/// otherwise copies every literal to RAM at boot).
namespace common_keys {
inline constexpr char req[]  NOTE_FLASH_ATTR = "req";
inline constexpr char cmd[]  NOTE_FLASH_ATTR = "cmd";
inline constexpr char id[]   NOTE_FLASH_ATTR = "id";
inline constexpr char body[] NOTE_FLASH_ATTR = "body";
} // namespace common_keys

} // namespace detail

} // namespace note
