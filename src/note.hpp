#pragma once
/// @file note.hpp
/// Single-include entry point for note-cpp.
///
/// On Arduino, this also serves as the gateway header that activates
/// the library (Arduino only adds src/ to the include path).
///
/// Usage:
///   #include <note.hpp>
///
/// This pulls in the typed API surface. On Arduino platforms, it also
/// includes the Arduino convenience header (serial/I2C begin() etc.).

#include "note/notecard.hpp"
#include "note/notecard_api.hpp"
#include "note/api.hpp"
#include "note/body.hpp"
#include "note/units.hpp"

#ifdef ARDUINO
#include "note/arduino.hpp"
#endif

// ── Default namespace imports ────────────────────────────────────────────────
// Import common names so user code reads naturally:
//   Notecard nc;             // instead of note::arduino::Notecard
//   nc.hub.set().outbound(15_mins).execute();
//
// Defaults: all enabled. Set NOTE_USING_NAMESPACE=0 to disable everything,
// or set individual flags to 0 for finer control.
#ifndef NOTE_USING_NAMESPACE
#define NOTE_USING_NAMESPACE 1
#endif
#ifndef NOTE_USING_LITERALS
#define NOTE_USING_LITERALS NOTE_USING_NAMESPACE
#endif
#ifndef NOTE_USING_ATTN
#define NOTE_USING_ATTN NOTE_USING_NAMESPACE
#endif
#ifndef NOTE_USING_SERIAL
#define NOTE_USING_SERIAL NOTE_USING_NAMESPACE
#endif
#ifndef NOTE_USING_TRIANGULATE
#define NOTE_USING_TRIANGULATE NOTE_USING_NAMESPACE
#endif

// Opt out:
//   #define NOTE_USING_NAMESPACE 0     — disable all imports
//   #define NOTE_USING_LITERALS 0      — disable duration literals only
//   #define NOTE_USING_ATTN 0          — disable attn flag constants
//   etc.

#if NOTE_USING_NAMESPACE

using namespace note;

#if NOTE_USING_LITERALS
using namespace note::literals;
#endif
#if NOTE_USING_ATTN
using namespace note::attn;
#endif
#if NOTE_USING_SERIAL
using namespace note::serial;
#endif
#if NOTE_USING_TRIANGULATE
using namespace note::triangulate;
#endif

#ifdef ARDUINO
// Concrete alias so unqualified `Notecard nc;` is unambiguous.
// `using note::arduino::Notecard;` imports the template name, but
// `using namespace note;` also makes `note::Notecard` (non-template)
// findable — GCC reports ambiguity. A type alias resolves it.
using Notecard = note::arduino::Notecard<>;
#endif

#endif // NOTE_USING_NAMESPACE
