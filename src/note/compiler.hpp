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
#include "note_config.hpp"
#ifndef NOTE_SHORT_ERRORS
#define NOTE_SHORT_ERRORS 0
#endif

#if NOTE_SHORT_ERRORS
#  define NOTE_ERR(msg) "E"
#else
#  define NOTE_ERR(msg) msg
#endif

// NOTE_ERR is used both for ErrorInfo::message construction AND for
// internal SAX/JSON parsers that return string_view error codes.
// Flash-ifying it globally would require migrating every SAX-internal
// caller. On AVR NOTE_MINIMAL already implies NOTE_SHORT_ERRORS, so
// the library's internal error literals collapse to "E" there —
// leaving only user-provided messages (rare) and the enum-name tables
// (now PROGMEM-backed via to_string(Error/Cause)) as RAM consumers.

// NOTE_SINK_NOINLINE — opt-in noinline marker for sink dispatch helpers.
// The make_sax_dispatch<SinkT> lambda is a 10-way switch that inlines each
// case body. For sinks that convert SaxEvent → BodyEvent and forward to a
// BodyHandler (notably GenericResponseSink), the inlined per-case BodyEvent
// construction dominates the lambda body — the dispatch lambda for one
// sink is ~1.3 KB on AVR. Outlining the switch via NOTE_SINK_NOINLINE
// shrinks the lambda to a forwarder; the body lives once in the helper.
// On single-sink builds the win is modest; on multi-sink builds each
// extra SinkT shrinks from ~1.3 KB to ~30 B.
#if defined(__GNUC__) || defined(__clang__)
#  define NOTE_SINK_NOINLINE __attribute__((noinline))
#else
#  define NOTE_SINK_NOINLINE
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
