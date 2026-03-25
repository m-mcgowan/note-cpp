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

#ifdef ARDUINO
#include "note/arduino.hpp"
#endif
