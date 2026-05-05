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
#include "note/static_notecard.hpp"
#include "note/api.hpp"
#include "note/body.hpp"
#include "note/json_buf.hpp"
#include "note/progmem.hpp"
#include "note/request_set.hpp"
#include "note/units.hpp"

#ifdef ARDUINO
// Always pull in the Arduino serial HAL + transport-stack helpers —
// these are the lightweight pieces that NOTE_MINIMAL builds (AVR-class)
// consume directly via `StaticNotecard<arduino::SerialTransportStack<>>`.
// On non-MINIMAL builds, `<note/arduino.hpp>` re-includes them and
// adds the `note::arduino::Notecard` wrapper class with
// `begin(Serial, ...)` helpers; that wrapper depends on
// `unique_ptr<JsonBackend>`, polymorphic transport, etc., most of which
// NOTE_MINIMAL strips, so we skip the wrapper there.
#include "note/arduino/serial.hpp"
#include "note/arduino/begin.hpp"
#if !NOTE_MINIMAL
#include "note/arduino.hpp"
#endif
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

#ifdef ARDUINO
// On Arduino, `Notecard` resolves to the Arduino subclass.
// We cannot `using namespace note;` here because it would also pull in
// `note::Notecard` (the transport-agnostic host class) and make the
// unqualified name ambiguous. Instead we expose the handful of names
// Arduino sketches commonly use; qualify the rest (`note::JsonBuf` etc.).
//
// `note::arduino::Notecard` is gated out of NOTE_MINIMAL builds (see
// the include block above), so the alias is similarly conditional.
// Such builds construct a `StaticNotecard<...>` directly and don't
// need the alias.
#if !NOTE_MINIMAL
#if __cplusplus >= 202002L
using Notecard = note::arduino::Notecard<>;
using note::template_of;
#else
using Notecard = note::arduino::Notecard;
#endif
#endif // !NOTE_MINIMAL
using note::printable;
using note::body;
#else
using namespace note;
#endif

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

#endif // NOTE_USING_NAMESPACE
