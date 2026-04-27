#pragma once
// Stub header that exists only to satisfy `#include <Wire.h>` in
// note/arduino/i2c.hpp during host-side arduino test builds.
//
// The actual TwoWire definition (and millis/delay etc.) lives in
// arduino_stubs.hpp, which is force-included at the top of every
// arduino test translation unit via -include.
