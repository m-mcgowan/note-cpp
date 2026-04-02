#pragma once

#include <note/note_config.hpp>

// Compiler portability utilities.
//
// NOTE_UNREACHABLE() — marks a code path as unreachable.
// Enables the compiler to elide the dead code and suppress "missing return"
// warnings. Behaviour is undefined if the path is actually reached.
//
// Dispatches to the best available mechanism:
//   C++23   : std::unreachable() (standard, <utility>)
//   GCC/Clang older: __builtin_unreachable()
//   MSVC    : __assume(false)
//   Fallback: no-op (safe, just no optimisation hint)

// NOTE_ERR(msg) — error message string literal.
// When NOTE_SHORT_ERRORS is 1, all error messages collapse to "E",
// saving ~960 bytes of RAM on AVR (Harvard architecture copies all
// string literals from flash to RAM at startup).
#ifndef NOTE_SHORT_ERRORS
#define NOTE_SHORT_ERRORS 0
#endif

#if NOTE_SHORT_ERRORS
#  define NOTE_ERR(msg) "E"
#else
#  define NOTE_ERR(msg) msg
#endif

#if defined(__cpp_lib_unreachable)
#  include <utility>
#  define NOTE_UNREACHABLE() ::std::unreachable()
#elif defined(__GNUC__) || defined(__clang__)
#  define NOTE_UNREACHABLE() __builtin_unreachable()
#elif defined(_MSC_VER)
#  define NOTE_UNREACHABLE() __assume(false)
#else
#  define NOTE_UNREACHABLE() (void)0
#endif
