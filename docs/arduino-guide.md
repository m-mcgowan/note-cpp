# Arduino Guide

Getting started with `note-cpp` on Arduino. Covers setup, printing,
string handling, and patterns specific to the Arduino environment.

## Setup

```cpp
#include <note/arduino.hpp>

note::arduino::Notecard nc;

void setup() {
    Serial.begin(115200);

    // Serial transport (most common)
    nc.begin(Serial1, 9600);

    // Or I2C transport
    // nc.begin(Wire);

    // Configure the Notecard
    nc.hub.set()
        .product("com.example.app")
        .mode("periodic")
        .execute();
}
```

## Printing Response Fields

Response fields are Arduino `Printable` — use `Serial.print()` directly:

```cpp
auto r = nc.card.version().execute();
if (r) {
    Serial.print("Version: ");
    Serial.println(r.version);    // Printable — just works
    Serial.print("Device: ");
    Serial.println(r.device);
}
```

Print the entire response as JSON:

```cpp
Serial.println(r);  // prints {"version":"...","device":"..."}
```

### Avoid Serial.printf with string_view

Arduino's `Serial.printf()` does not support `string_view`. This is
a common source of confusion when migrating from note-c:

```cpp
// ❌ Wrong — string_view is not null-terminated, %s may read garbage
Serial.printf("ver=%s\n", r.version.data());

// ❌ Awkward — works but verbose
Serial.printf("ver=%.*s\n", (int)r.version.size(), r.version.data());

// ✅ Correct — use Serial.print()
Serial.print("ver=");
Serial.println(r.version);
```

## String Conversion

`OwnedBuffer` (returned by `transact()`) converts implicitly to
Arduino `String`:

```cpp
auto r = nc.transact(R"({"req":"card.version"})");
if (r) {
    String json = *r;             // implicit conversion
    Serial.println(json);
}
```

Response fields (`string_view`) can be converted explicitly:

```cpp
auto r = nc.card.version().execute();
if (r) {
    String ver(r.version.data(), r.version.size());
}
```

## Printing Results

`Result<T>` has a non-virtual `printTo()` method that prints the value
on success or the error message on failure:

```cpp
auto r = nc.card.version().execute();
r.printTo(Serial);   // prints response or "Error: ..."
Serial.println();
```

When you need the Arduino `Printable` interface (e.g., for
`Serial.println()`), wrap with `printable()`:

```cpp
Serial.println(note::printable(r));
```

`printable()` is a zero-copy wrapper — it holds a reference to the
original result, no allocation.

## Error Handling

```cpp
auto r = nc.hub.set().product("com.example").execute();
if (!r) {
    Serial.print("Error: ");
    Serial.println(r.error());  // ErrorInfo is Printable
    return;
}
```

## Units and Literals

Time literals work naturally with request fields:

```cpp
using namespace note::literals;

nc.hub.set()
    .outbound(5_mins)
    .inbound(60_mins)
    .execute();

nc.card.attn().arm(note::attn::files)
    .seconds(120_s)
    .execute();
```

Available literals: `_s` (seconds), `_mins` / `_minutes`, `_hours`, `_days`.

## Raw JSON Passthrough

For serial passthrough protocols or debug consoles:

```cpp
// Auto-sized buffer (uses heap)
auto r = nc.transact(R"({"req":"card.version"})");
if (r) {
    Serial.println(*r);          // OwnedBuffer is Printable
    String json = *r;            // or convert to String
}

// Explicit buffer (zero-alloc)
char buf[512];
auto r2 = nc.transact(R"({"req":"card.version"})", buf);
if (r2) {
    Serial.println(r2->data());  // string_view into buf
}

// Standalone passthrough (no allocator needed)
note::BareNotecard bare(streaming_transport);
char buf[512];
auto r3 = bare.transact(R"({"req":"card.version"})", buf);
```

## ATTN Pin (Interrupts)

```cpp
// Arm for file changes
nc.card.attn().arm(note::attn::files)
    .seconds(300)
    .execute();

// In your ATTN interrupt handler — re-arm (idempotent)
nc.card.attn().rearm(note::attn::files)
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

// Disable/enable ATTN processing
nc.card.attn().off().execute();
nc.card.attn().on().execute();
```

## AVR (ATmega328P)

For severely constrained platforms, use `NOTE_MINIMAL` to strip
all optional features:

```ini
; platformio.ini
build_flags = -DNOTE_MINIMAL
```

This sets: `NOTE_NO_BUFFERED`, `NOTE_NO_STD_STRING`, `NOTE_NO_MD5`,
`NOTE_NO_CRC`, `NOTE_PRINTABLE=0`, `NOTE_EXTRAS=0`, `NOTE_SHORT_ERRORS=1`.

Use `StaticNotecard` for zero-vtable dispatch:

```cpp
#include <note/static_notecard.hpp>
#include <note/arduino/serial.hpp>

using SerialNotecard = note::StaticNotecard<note::arduino::SerialTransportStack<>>;

char arena_pool[64];
note::MonotonicArena arena(arena_pool);
SerialNotecard nc(note::arena_allocator(arena), Serial1, 9600);
note::Api api(nc);

api.hub.set().product("com.example").execute();
```

Binary size: 14.6 KB flash (45%), 712 bytes RAM (35%) on ATmega328P —
41% smaller than note-c.
