#pragma once

/// @file debug.hpp
/// Debug observability for Notecard communication.
///
/// DebugListener is a struct of function pointers — when all null (default),
/// the compiler eliminates all debug code paths at -O1+. Per-instance,
/// not global. Follows the Allocator pattern.
///
/// Usage:
///   nc.set_debug(note::arduino::debug(Serial));  // enable wire tracing
///   nc.clear_debug();                                    // disable
///
/// For guaranteed zero overhead in production:
///   note::Notecard<note::NoDebug> nc(transport);  // no set_debug() available

#include "types.hpp"

#include <cstddef>
#include <cstdint>

namespace note {

// ── Structured event types ─────────────────────────────────────────────

/// Wire data direction.
enum class WireDirection : uint8_t { Send, Receive };

/// Structured wire event — the raw JSON bytes sent or received.
struct WireEvent {
    string_view json;
    WireDirection direction;
};

/// Timing event markers — emitted immediately before/after the action.
/// The listener captures its own timestamp (e.g. millis()) on receipt.
enum class TimingEvent : uint8_t {
    // Request lifecycle (end-to-end)
    BuildBegin,           // about to serialize request JSON
    BuildEnd,             // JSON serialization complete
    TransactionBegin,     // about to send request to transport
    TransmitBegin,        // about to write bytes to HAL
    TransmitEnd,          // HAL write complete
    ReceiveBegin,         // waiting for response bytes
    ReceiveEnd,           // response line/SAX parse complete
    ParseBegin,           // about to parse response (buffered: tree parse)
    ParseEnd,             // response parsed into typed fields
    TransactionEnd,       // full round-trip complete (or error)

    // Transport-level
    RetryBegin,           // about to retry after failure
    ResetBegin,           // about to reset transport
    ResetEnd,             // reset complete
    SegmentDelay,         // inter-segment pacing delay starting
};

/// Transport-level structured events.
enum class TransportEvent : uint8_t {
    Retry,           // retrying after failure (detail = attempt number)
    ResetFailed,     // HAL reset did not succeed
    CrcMismatch,     // response CRC didn't match
    Timeout,         // response timeout
    SendFailed,      // HAL transmit failed
};

// ── DebugListener ──────────────────────────────────────────────────────

/// Function-pointer struct for debug callbacks. All default to nullptr.
/// When unused, the compiler eliminates dead branches after inlining.
struct DebugListener {
    void (*on_wire)(const WireEvent& event, void* ctx) = nullptr;
    void (*on_alloc)(void* ptr, size_t size, void* ctx) = nullptr;
    void (*on_free)(void* ptr, size_t size, void* ctx) = nullptr;
    void (*on_realloc)(void* old_ptr, void* new_ptr,
                       size_t old_n, size_t new_n, void* ctx) = nullptr;
    void (*on_timing)(TimingEvent event, string_view request_type, void* ctx) = nullptr;
    void (*on_transport)(TransportEvent event, uint32_t detail, void* ctx) = nullptr;
    void* ctx = nullptr;
};

// ── Inline call-site helpers ───────────────────────────────────────────
// These check the pointer before calling. When the pointer is null,
// the entire call (including argument evaluation) is eliminated.
// When NOTE_DEBUG_ENABLED is 0, all calls are no-ops (zero code).

#if NOTE_DEBUG_ENABLED

inline void debug_wire(const DebugListener& d, string_view json, WireDirection dir) {
    if (d.on_wire) d.on_wire({json, dir}, d.ctx);
}

inline void debug_timing(const DebugListener& d, TimingEvent ev, string_view req = {}) {
    if (d.on_timing) d.on_timing(ev, req, d.ctx);
}

inline void debug_transport(const DebugListener& d, TransportEvent ev, uint32_t detail = 0) {
    if (d.on_transport) d.on_transport(ev, detail, d.ctx);
}

inline void debug_alloc(const DebugListener& d, void* ptr, size_t size) {
    if (d.on_alloc) d.on_alloc(ptr, size, d.ctx);
}

inline void debug_free(const DebugListener& d, void* ptr, size_t size) {
    if (d.on_free) d.on_free(ptr, size, d.ctx);
}

inline void debug_realloc(const DebugListener& d, void* old_ptr, void* new_ptr,
                           size_t old_n, size_t new_n) {
    if (d.on_realloc) d.on_realloc(old_ptr, new_ptr, old_n, new_n, d.ctx);
}

#else // !NOTE_DEBUG_ENABLED — no-op stubs, zero code generation

inline void debug_wire(const DebugListener&, string_view, WireDirection) {}
inline void debug_timing(const DebugListener&, TimingEvent, string_view = {}) {}
inline void debug_transport(const DebugListener&, TransportEvent, uint32_t = 0) {}
inline void debug_alloc(const DebugListener&, void*, size_t) {}
inline void debug_free(const DebugListener&, void*, size_t) {}
inline void debug_realloc(const DebugListener&, void*, void*, size_t, size_t) {}

// Also accept NoDebug (used by Protocol when NOTE_DEBUG_ENABLED=0).
struct NoDebug;
inline void debug_wire(const NoDebug&, string_view, WireDirection) {}
inline void debug_timing(const NoDebug&, TimingEvent, string_view = {}) {}
inline void debug_transport(const NoDebug&, TransportEvent, uint32_t = 0) {}
inline void debug_alloc(const NoDebug&, void*, size_t) {}

#endif // NOTE_DEBUG_ENABLED

// ── Debug policies ─────────────────────────────────────────────────────

/// NoDebug — compile-time zero debug. All methods are constexpr no-ops.
/// [[no_unique_address]] makes this zero bytes in the containing struct.
struct NoDebug {
    static constexpr void wire(string_view, WireDirection) {}
    static constexpr void timing(TimingEvent, string_view = {}) {}
    static constexpr void transport(TransportEvent, uint32_t = 0) {}
    static constexpr void alloc(void*, size_t) {}
    static constexpr void free(void*, size_t) {}
    static constexpr void realloc(void*, void*, size_t, size_t) {}
};

#if NOTE_DEBUG_ENABLED
/// RuntimeDebug — wraps DebugListener. Zero overhead when all null.
struct RuntimeDebug {
    DebugListener listener{};

    void set(DebugListener d) { listener = d; }
    void clear() { listener = {}; }

    void wire(string_view json, WireDirection dir) const { debug_wire(listener, json, dir); }
    void timing(TimingEvent ev, string_view req = {}) const { debug_timing(listener, ev, req); }
    void transport(TransportEvent ev, uint32_t detail = 0) const { debug_transport(listener, ev, detail); }
    void alloc(void* ptr, size_t size) const { debug_alloc(listener, ptr, size); }
    void free(void* ptr, size_t size) const { debug_free(listener, ptr, size); }
    void realloc(void* old_ptr, void* new_ptr, size_t old_n, size_t new_n) const {
        debug_realloc(listener, old_ptr, new_ptr, old_n, new_n);
    }
};
#endif // NOTE_DEBUG_ENABLED

// ── Debug category flags ───────────────────────────────────────────────
// Used by convenience adapters (e.g. arduino::debug) to select
// which categories are enabled.

inline constexpr uint8_t DebugWire     = 1u << 0;
inline constexpr uint8_t DebugTiming   = 1u << 1;
inline constexpr uint8_t DebugMemory   = 1u << 2;
inline constexpr uint8_t DebugTransport = 1u << 3;
inline constexpr uint8_t DebugAll      = 0xFF;

} // namespace note
