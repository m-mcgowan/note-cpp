#pragma once

/// @file progmem.hpp
/// Flash string helpers for Harvard architecture platforms (AVR).
///
/// On AVR, string literals in .data consume RAM. These helpers let
/// generated code declare field name strings in flash (PROGMEM) and
/// access them without macros. On non-Harvard platforms, all functions
/// are trivial pass-throughs that the compiler optimizes away.

#include <note/types.hpp>
#include <cstring>

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

namespace note {

/// A string stored in program memory (flash). On non-Harvard platforms,
/// this is just a regular string pointer — zero overhead.
struct FlashString {
    const char* ptr;
    size_t len;

    /// Compare against a RAM string_view.
    bool operator==(string_view sv) const {
#if NOTE_PROGMEM
        return sv.size() == len && memcmp_P(sv.data(), ptr, len) == 0;
#else
        return sv == string_view(ptr, len);
#endif
    }

    /// Copy to a RAM buffer and return a string_view.
    /// buf must be at least len bytes.
    string_view to_view(char* buf) const {
#if NOTE_PROGMEM
        memcpy_P(buf, ptr, len);
#else
        memcpy(buf, ptr, len);
#endif
        return {buf, len};
    }

    /// On non-Harvard, return a string_view directly (no copy needed).
    /// On Harvard, this is UB — use to_view() instead.
#if !NOTE_PROGMEM
    string_view view() const { return {ptr, len}; }
#endif
};

/// Create a FlashString from a string literal declared with NOTE_FLASH.
/// On AVR, the literal must be in PROGMEM.
template<size_t N>
constexpr FlashString flash(const char (&s)[N]) {
    return {s, N - 1};
}

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
void add_raw_flash(Builder& b, FlashString key, string_view value) {
#if NOTE_PROGMEM
    char buf[64];
    b.add_raw(key.to_view(buf), value);
#else
    b.add_raw(key.view(), value);
#endif
}

} // namespace note

/// Attribute for declaring a string in program memory.
/// Usage: static const char name[] NOTE_FLASH_ATTR = "value";
#if NOTE_PROGMEM
#  define NOTE_FLASH_ATTR PROGMEM
#else
#  define NOTE_FLASH_ATTR
#endif
