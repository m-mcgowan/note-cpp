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
// Opt out with:
//   #define NOTE_NO_USING_NAMESPACE 1    — disable all imports
//   #define NOTE_NO_USING_LITERALS  1    — disable literals only

#ifndef NOTE_NO_USING_NAMESPACE

#ifndef NOTE_NO_USING_LITERALS
using namespace note::literals;
using namespace note::attn;
using namespace note::serial;
using namespace note::triangulate;
#endif

#ifdef ARDUINO
using note::arduino::Notecard;
#endif

#endif // NOTE_NO_USING_NAMESPACE
