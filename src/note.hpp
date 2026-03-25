#pragma once
// Gateway header for Arduino Library Manager.
//
// Arduino adds only src/ to the include path. Including this header
// activates the library, making all nested headers (note/api.hpp,
// note/arduino.hpp, etc.) available via their full paths.
//
// Usage in Arduino sketches:
//   #include <note.hpp>               // activate library
//   #include <note/arduino.hpp>       // then include what you need

// Pull in the core Notecard API so a single #include is enough
// for simple sketches.
#include "note/notecard.hpp"
#include "note/api.hpp"
