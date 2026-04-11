# Arduino Guide

## Setup

```cpp
#include <note.hpp>

Notecard nc;

void setup() {
    Serial.begin(115200);

    // Serial transport
    nc.begin(Serial1, 9600);

    // Or I2C transport
    // nc.begin(Wire);

    nc.hub.set()
        .product("com.example.app")
        .mode("periodic")
        .execute();
}
```

`note.hpp` imports `Notecard`, duration literals (`15_mins`, `5_s`), and
other common names into the global namespace. See
[namespace imports](../../feature-flags.md#namespace-imports) to customize.

## Printing

Most `note-cpp` types are Arduino `Printable` — use `Serial.print()` directly:

| Type | Example | Printable? |
|------|---------|:----------:|
| Response fields | `Serial.println(r.version)` | yes |
| Errors | `Serial.println(r.error())` | yes |
| Full responses | `Serial.println(printable(r))` | via `printable()` |
| Request fields | `Serial.println(printable(req.product))` | via `printable()` |

```cpp
auto r = nc.card.version().execute();
if (r) {
    Serial.print("Version: ");
    Serial.println(r.version);
    Serial.print("Device: ");
    Serial.println(r.device);
} else {
    Serial.println(r.error());
}
```

For `ApiResult`, use the `printable()` wrapper:

```cpp
auto r = nc.card.version().execute();
Serial.println(printable(r));  // prints response or error
```

## String fields

Response string fields use `string_view` internally — a lightweight
reference to the response data. You don't need to know this for normal
use since `Serial.print()` handles it directly.

When you need an Arduino `String` (e.g. to store or pass to other
libraries), convert explicitly:

```cpp
auto r = nc.card.version().execute();
if (r) {
    String ver(r.version.data(), r.version.size());
}
```

> **Don't use `Serial.printf("%s", ...)` with response fields** —
> they may not be null-terminated. Use `Serial.print()` instead.

## Error Handling

```cpp
auto r = nc.hub.set().product("com.example").execute();
if (!r) {
    Serial.println(r.error());
    return;
}
```

For more detail:

```cpp
auto err = r.error();
// err.code:    Error::Notecard, SendFailed, etc.
// err.cause:   Cause::Timeout, HalError, etc.
// err.message: the Notecard's error string
Serial.println(err);
```

See [Error Handling](../../error-handling.md) for the full reference.

## Duration Literals

Time literals are available without `using` declarations (imported
by `note.hpp`):

```cpp
nc.hub.set()
    .outbound(5_mins)
    .inbound(60_mins)
    .execute();

nc.card.attn().arm()
    .connected()
    .seconds(120_s)
    .execute();
```

Available: `_s` / `_seconds`, `_mins` / `_minutes`, `_hours`, `_days`.

## ATTN Pin (Interrupts)

```cpp
nc.card.attn().arm()
    .connected()
    .files()
    .seconds(300)
    .execute();

// Query what triggered ATTN
auto q = nc.card.attn().query().execute();
if (q) {
    for (auto& trigger : q.files) {
        Serial.print("  trigger: ");
        Serial.println(trigger);
    }
}

// Disable/enable
nc.card.attn().off().execute();
nc.card.attn().on().execute();
```

See the [ATTN guide](../../guides/card-attn-guide.md) for interrupt wiring and
sleep patterns.

## AVR (ATmega328P)

For severely constrained targets, use `NOTE_MINIMAL`:

```ini
; platformio.ini
build_flags = -DNOTE_MINIMAL
```

This strips buffered JSON, polymorphic dispatch, and optional features.
Use `StaticNotecard` for zero-vtable, zero-heap operation:

```cpp
#include <note/static_notecard.hpp>
#include <note/api.hpp>
#include <note/arduino/begin.hpp>

char arena_buf[64];
note::MonotonicArena arena(arena_buf);

using SerialNotecard = note::StaticNotecard<note::arduino::SerialTransportStack<>>;
SerialNotecard nc(note::arena_allocator(arena), Serial, 9600);
note::Api api(nc);

api.hub.set().product("com.example").execute();
```

See [feature flags](../../feature-flags.md) for all compile-time options.
