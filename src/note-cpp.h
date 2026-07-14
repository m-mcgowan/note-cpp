#pragma once
/// @file note-cpp.h
/// Canonical library header for note-cpp.
///
/// Named after the library (note-cpp) so the Arduino Library Manager and IDE
/// can auto-include it. It simply forwards to the single-include entry point,
/// note.hpp — either header works and pulls in the full typed API surface.
///
/// Usage:
///   #include <note-cpp.h>   // or, equivalently, #include <note.hpp>

#include "note.hpp"
