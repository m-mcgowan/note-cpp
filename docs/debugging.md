# Debugging

note-cpp provides structured debug observability: wire data, timing,
memory, and transport events. Zero overhead when unused.

## Quick Start

```cpp
// Arduino — one line to see all wire traffic
nc.setDebugOutput(Serial);

// Output:
// >> {"req":"hub.set","product":"com.example"}
// << {}
// >> {"req":"card.version"}
// << {"version":"notecard-7.2.1","device":"dev:123"}
```

## Three Modes

### 1. Default — runtime debug available (recommended)

Debug callbacks are available but dormant. Call `set_debug()` at any
time to activate. When inactive (all function pointers null), the
optimizer eliminates the debug code paths at -O1+.

```cpp
note::Notecard nc(transport);        // RuntimeDebug — dormant
nc.set_debug(listener);              // activate
nc.clear_debug();                    // deactivate
```

This is the right choice for ESP32, STM32, and most platforms where
you may want debug output during development but not in production.

### 2. Production — compile-time no-debug

For guaranteed zero overhead, use `NoDebug`:

```cpp
note::Notecard<note::NoDebug> nc(transport);
// nc.set_debug() is not available — debug is compiled out
```

No function pointers, no branches, no code generated.

### 3. AVR with NOTE_MINIMAL

`NOTE_MINIMAL` sets `NOTE_DEBUG_ENABLED=0` by default, selecting
NoDebug to save flash. To enable debug on AVR for development:

```ini
build_flags = -DNOTE_MINIMAL -DNOTE_DEBUG_ENABLED=1
```

## Debug Categories

Select what you want to see with category flags:

```cpp
// Arduino convenience — wire only (default)
nc.setDebugOutput(Serial);

// Wire + timing
nc.setDebugOutput(Serial, note::DebugWire | note::DebugTiming);

// Everything
nc.setDebugOutput(Serial, note::DebugAll);
```

| Flag | Prefix | What it shows |
|------|--------|---------------|
| `DebugWire` | `>> / <<` | Raw JSON request/response bytes |
| `DebugTiming` | `[T]` | Lifecycle events: build, transmit, receive, parse |
| `DebugMemory` | `[M]` | Allocations and frees (pointer + size) |
| `DebugTransport` | `[!]` | Retries, CRC mismatches, timeouts, send failures |

## Custom Listeners

For structured debug handling (logging to SD card, sending to a
dashboard, etc.), create a `DebugListener`:

```cpp
note::DebugListener d;
d.ctx = &my_logger;

d.on_wire = [](const note::WireEvent& ev, void* ctx) {
    auto* log = static_cast<MyLogger*>(ctx);
    log->write(ev.direction == note::WireDirection::Send ? "TX" : "RX",
               ev.json);
};

d.on_timing = [](note::TimingEvent ev, note::string_view req, void* ctx) {
    auto* log = static_cast<MyLogger*>(ctx);
    log->timestamp(ev, req);
};

nc.set_debug(d);
```

### Timing Events

Timing events are markers — the listener captures its own timestamp
(e.g. `millis()`) on receipt. This decouples the library from any
specific clock source.

```cpp
d.on_timing = [](note::TimingEvent ev, note::string_view req, void*) {
    static uint32_t t;
    if (ev == note::TimingEvent::TransactionBegin) t = millis();
    if (ev == note::TimingEvent::TransactionEnd)
        Serial.printf("%.*s: %lums\n", (int)req.size(), req.data(), millis() - t);
};
```

Full event sequence for a tree-path request:

```
TransactionBegin  → BuildBegin → BuildEnd
                  → TransmitBegin → TransmitEnd
                  → ReceiveBegin → ReceiveEnd
                  → ParseBegin → ParseEnd
                  → TransactionEnd
```

On retry:
```
RetryBegin → ResetBegin → ResetEnd → TransmitBegin → ...
```

### Transport Events

Structured events for protocol-level diagnostics:

| Event | Detail | When |
|-------|--------|------|
| `Retry` | attempt number | Before each retry |
| `ResetFailed` | 0 | Transport init failed |
| `CrcMismatch` | attempt number | Response CRC didn't match |
| `Timeout` | attempt number | Response read timed out |
| `SendFailed` | attempt number | HAL transmit failed |

### Memory Events

Track allocator activity (useful for heap profiling):

```cpp
d.on_alloc = [](void* ptr, size_t size, void*) {
    Serial.printf("[M] alloc %u @ %p\n", size, ptr);
};
d.on_free = [](void* ptr, size_t size, void*) {
    Serial.printf("[M] free %u @ %p\n", size, ptr);
};
```

## Zero Overhead

When no debug listener is registered (the default), all debug call
sites compile to a null-pointer check that the optimizer eliminates:

```cpp
// In the library:
inline void debug_wire(const DebugListener& d, string_view json, WireDirection dir) {
    if (d.on_wire) d.on_wire({json, dir}, d.ctx);  // eliminated when null
}
```

With `NoDebug` policy, even the null check is eliminated — the
methods are `constexpr` no-ops that produce zero code.

## BareNotecard

`BareNotecard` does not have debug hooks — it's a minimal raw transport
wrapper. To debug passthrough traffic, use `Notecard::transact()` which
goes through the debug-instrumented path.
