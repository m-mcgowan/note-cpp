#pragma once
/// @file note.hpp
/// Convenience umbrella — one include for typical host-side use.
///
///     #include <note/note.hpp>
///
/// Pulls in:
///   - the typed API (api.hpp transitively brings Notecard, transport,
///     NOTE_FIELDS, StreamingTransport, Allocator, duration units, etc.)
///   - compile-time JSON helpers (note::json<lambda>, JsonBuf, json_fmt)
///
/// Platform HALs stay in their own umbrellas — see:
///   - <note/arduino.hpp> for Arduino (HardwareSerial / TwoWire)
///   - <note/posix.hpp>   for Linux / macOS / BSD hosts
///
/// Pick individual headers (api.hpp, json_buf.hpp, etc.) directly when
/// you want minimal include graph — e.g. on flash-constrained AVR
/// builds where every KB matters.

#include <note/api.hpp>        // typed API + Notecard + streaming transport
#include <note/json_buf.hpp>   // compile-time JSON: note::json<lambda>() and JsonBuf<N>
#include <note/json_fmt.hpp>   // compile-time JSON template + runtime values
