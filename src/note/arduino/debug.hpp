#pragma once

/// @file arduino/debug.hpp
/// Arduino debug adapter — routes wire events to a Print stream.
///
/// Usage:
///   nc.setDebugOutputStream(Serial);  // note-c compatible
///   // or
///   nc.set_debug(note::arduino::serial_debug(Serial));  // explicit

#ifdef ARDUINO

#include "../debug.hpp"

namespace note::arduino {

/// Create a DebugListener that prints wire data to an Arduino Stream.
/// Output format matches note-c: ">> {json}\n" for sent, "<< {json}\n" for received.
inline DebugListener serial_debug(Print& out) {
    DebugListener d;
    d.ctx = &out;
    d.on_wire = [](const WireEvent& ev, void* ctx) {
        auto* p = static_cast<Print*>(ctx);
        p->print(ev.direction == WireDirection::Send ? ">> " : "<< ");
        p->write(reinterpret_cast<const uint8_t*>(ev.json.data()), ev.json.size());
        p->println();
    };
    return d;
}

} // namespace note::arduino

#endif // ARDUINO
