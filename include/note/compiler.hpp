#pragma once

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
