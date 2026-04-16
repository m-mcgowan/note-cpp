#pragma once
/// @file arduino/compat.hpp
/// Arduino compatibility: include Arduino.h and clean up macro pollution.
///
/// Arduino.h defines min, max, abs, round, etc. as macros which collide
/// with standard C++ names. Include this header instead of Arduino.h
/// directly to get a clean environment.
///
/// This header is only effective when ARDUINO is defined. On non-Arduino
/// platforms it is a no-op.

#ifdef ARDUINO

// NOTE_ARDUINO_STUBS: test environments provide their own Print/Printable
// stubs and define this before including note headers.
#if !NOTE_ARDUINO_STUBS
#include <Arduino.h>
#endif

// Arduino defines these as macros, breaking std::min, std::max, etc.
#undef min
#undef max
#undef abs
#undef round

#endif // ARDUINO
