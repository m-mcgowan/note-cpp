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

namespace detail {
    struct DebugCtx {
        Print* out;
        uint8_t flags;
    };
}

/// Create a DebugListener that prints to an Arduino Print.
/// flags selects which categories: DebugWire (default), DebugTiming,
/// DebugMemory, DebugTransport, or DebugAll.
///
///   nc.setDebugOutput(Serial);                          // wire only
///   nc.setDebugOutput(Serial, note::DebugWire | note::DebugTiming);
inline DebugListener serial_debug(Print& out, uint8_t flags = DebugWire) {
    // Static context — one per call. Safe for single-Notecard usage.
    // For multiple Notecards with different debug streams, use set_debug() directly.
    static detail::DebugCtx ctx;
    ctx = {&out, flags};

    DebugListener d;
    d.ctx = &ctx;

    if (flags & DebugWire) {
        d.on_wire = [](const WireEvent& ev, void* c) {
            auto* p = static_cast<detail::DebugCtx*>(c)->out;
            p->print(ev.direction == WireDirection::Send ? ">> " : "<< ");
            p->write(reinterpret_cast<const uint8_t*>(ev.json.data()), ev.json.size());
            p->println();
        };
    }

    if (flags & DebugTiming) {
        d.on_timing = [](TimingEvent ev, string_view req, void* c) {
            auto* p = static_cast<detail::DebugCtx*>(c)->out;
            p->print("[T] ");
            // Print event name
            switch (ev) {
                case TimingEvent::BuildBegin:       p->print("build+"); break;
                case TimingEvent::BuildEnd:         p->print("build-"); break;
                case TimingEvent::TransactionBegin: p->print("txn+"); break;
                case TimingEvent::TransmitBegin:    p->print("tx+"); break;
                case TimingEvent::TransmitEnd:      p->print("tx-"); break;
                case TimingEvent::ReceiveBegin:     p->print("rx+"); break;
                case TimingEvent::ReceiveEnd:       p->print("rx-"); break;
                case TimingEvent::ParseBegin:       p->print("parse+"); break;
                case TimingEvent::ParseEnd:         p->print("parse-"); break;
                case TimingEvent::TransactionEnd:   p->print("txn-"); break;
                case TimingEvent::RetryBegin:       p->print("retry"); break;
                case TimingEvent::ResetBegin:       p->print("reset+"); break;
                case TimingEvent::ResetEnd:         p->print("reset-"); break;
                case TimingEvent::SegmentDelay:     p->print("seg_delay"); break;
            }
            if (!req.empty()) {
                p->print(" ");
                p->write(reinterpret_cast<const uint8_t*>(req.data()), req.size());
            }
            p->println();
        };
    }

    if (flags & DebugTransport) {
        d.on_transport = [](TransportEvent ev, uint32_t detail, void* c) {
            auto* p = static_cast<detail::DebugCtx*>(c)->out;
            p->print("[!] ");
            switch (ev) {
                case TransportEvent::Retry:       p->print("retry #"); p->print(detail); break;
                case TransportEvent::ResetFailed: p->print("reset failed"); break;
                case TransportEvent::CrcMismatch: p->print("CRC mismatch"); break;
                case TransportEvent::Timeout:     p->print("timeout"); break;
                case TransportEvent::SendFailed:  p->print("send failed"); break;
            }
            p->println();
        };
    }

    if (flags & DebugMemory) {
        d.on_alloc = [](void* ptr, size_t size, void* c) {
            auto* p = static_cast<detail::DebugCtx*>(c)->out;
            p->print("[M] alloc "); p->print((unsigned long)size);
            p->print(" @ 0x"); p->println((unsigned long)(uintptr_t)ptr, HEX);
        };
        d.on_free = [](void* ptr, size_t size, void* c) {
            auto* p = static_cast<detail::DebugCtx*>(c)->out;
            p->print("[M] free "); p->print((unsigned long)size);
            p->print(" @ 0x"); p->println((unsigned long)(uintptr_t)ptr, HEX);
        };
    }

    return d;
}

} // namespace note::arduino

#endif // ARDUINO
