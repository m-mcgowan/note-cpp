# Arduino guide

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
| Array elements | `Serial.println(printable(f))` | via `printable()` |
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

> Response string fields are null-terminated (via `StringPool::intern()`),
> so `Serial.printf("%s", r.version.c_str())` is safe. `Serial.print(r.version)`
> is preferred for typed output; use `printf` for format strings, or
> `.c_str()` / `.data()` when a `const char*` is needed.

## Error handling

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

## Duration literals

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

## ATTN pin (interrupts)

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
        Serial.println(printable(trigger));
    }
}

// Disable/enable
nc.card.attn().off().execute();
nc.card.attn().on().execute();
```

See the [ATTN guide](card-attn-guide.md) for interrupt wiring and
sleep patterns.

## AVR (ATmega328P)

For severely constrained targets, use `NOTE_MINIMAL`:

```ini
; platformio.ini
build_flags = -DNOTE_MINIMAL
```

This strips tree-mode JSON, polymorphic dispatch, and optional features.
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

### Binary size comparison

A realistic 8-endpoint app (hub.set, note.template, card.temp, note.add,
card.status, card.voltage, note.get with body parse, env.get) measured
on ATmega328P (Arduino Uno, 32 KB flash, 2 KB RAM):

Each row below peels off one layer of abstraction — showing how
much flash (and RAM) you get back by dropping to a lower-level API.
Rows are ordered from "best developer experience" down to "smallest
possible footprint":

| # | Style (typical call site) | Flash | Δ flash vs typed | RAM | Δ RAM vs typed |
|---|---|---|---|---|---|
| — | **note-c** (`Notecard::requestAndResponse(...)`) | 25,076 B | +346 B | 729 B | −107 B* |
| 1 | **typed api groups** — `api.hub.set().product(...).execute()` | 24,730 B | baseline | 836 B | baseline |
| 2 | **typed direct** — `nc.execute(HubSet{...})` | 24,520 B | **−210 B** | 804 B | −32 B |
| 3 | **raw + SAX sink** — `JsonBuf` + `transact_dispatch` + `JsonSink` | 20,528 B | **−4,202 B** | 848 B | +12 B |
| 4 | **raw + `JsonView` scan** (RAM keys) | 10,914 B | **−13,816 B** | 696 B | −140 B |
| 5 | **raw + `JsonView` scan** (`F()` flash keys) | **10,882 B** | **−13,848 B** | **680 B** | **−156 B** |

*note-c's RAM number excludes its ~371 B heap peak; all note-cpp
variants use zero heap.

Choosing a style:

1. **Typed api groups** — best DX. Use when ~25 KB flash + 800–900 B
   RAM fits your target.
2. **Typed direct** — identical API surface but constructs request
   objects manually. Tiny win over (1), used when you want explicit
   control of the request struct.
3. **Raw + SAX sink** — drops the codegen'd request/response types
   but keeps the SAX parser for robust decoding. The streaming sink
   means no response buffer in RAM — pick this when RAM is the
   bottleneck **and** the response may be too large to buffer.
4. **Raw + `JsonView` scan** — buffers the response and extracts
   known fields via substring search. **Biggest single step-down**:
   skips the SAX machinery entirely (≈8 KB flash). Pick this when
   flash is the bottleneck and response shapes are known ahead of
   time.
5. **Flash keys** — same as (4) but with scan keys in PROGMEM. A
   small additional RAM win on AVR; essentially free on other cores.

See the "Parsing responses" sections below for the code patterns
that correspond to rows 3, 4, and 5.

Build a request either way with `JsonBuf`:

```cpp
#include <note/static_notecard.hpp>
#include <note/arduino/begin.hpp>
#include <note/json_buf.hpp>

note::JsonBuf<128> req;
req.add("req", "note.add");
req.add("file", "sensors.qo");
req.begin_object("body");
    req.add("temp", 22.5);
    req.add("humidity", 60);
req.end_object();
req.close();

char rsp[64];
nc.stack().transport.transact_raw(req.view(), rsp, sizeof(rsp), 10000);
```

#### Parsing responses — SAX sink (low RAM, ~8 KB flash)

Define a `JsonSink` and call `transact_dispatch`. The response
streams through character-by-character — no buffer needed:

```cpp
struct BodySink : note::JsonSink {
    float temp = 0; int32_t humidity = 0;
    int depth = 0;
    void on_object_begin(note::string_view k) override {
        if (k == "body") depth = 1; else if (depth) ++depth;
    }
    void on_object_end(note::string_view) override { if (depth) --depth; }
    void on_number(note::string_view k, note::string_view raw) override {
        if (depth != 1) return;
        if (k == "temp") temp = static_cast<float>(note::parse_double(raw));
        else if (k == "humidity") humidity = static_cast<int32_t>(note::parse_int(raw));
    }
};

BodySink sink;
note::detail::NcErrorCapture err;
nc.stack().transport.transact_dispatch(
    [](note::JsonBuilder& b, void*) {
        b.add("req", "note.get");
        b.add("file", "config.qi");
    }, nullptr,
    note::make_sax_dispatch(sink), 10000, err);
```

#### Parsing responses — `JsonView` scan (low flash, needs response buffer)

Buffer the response with `transact_raw`, then scan known fields out
of the buffer with `note::JsonView`. No SAX parser compiled in:

```cpp
#include <note/json_view.hpp>

char rsp[128];
auto body = note::JsonView(
    nc.stack().transport.transact_raw(req.view(), rsp, sizeof(rsp), 10000)
).object("body");

float temp   = body.get_float("temp");
int32_t hum  = body.get_int("humidity");
```

`JsonView` unwraps `Result<string_view>` directly — if the transport
call errored, `body` is an empty view and the subsequent lookups
return their defaults, so best-effort extraction doesn't need an
explicit `if (resp)` check.

##### Saving RAM on AVR — flash-resident keys

On Arduino AVR, every string literal you pass as a key (e.g.
`"temperature"`) lives in both flash **and** RAM — AVR can't
execute from `.rodata`, so initialized literals occupy a copy in
each. On a 2 KB Uno, a handful of 12-character keys adds up.

`JsonView` and `note::scan::*` accept keys from program memory,
letting you keep them out of RAM:

```cpp
#include <note/json_view.hpp>
#include <note/progmem.hpp>

// Arduino ergonomic form — F("...") forces the string into PROGMEM
// and converts implicitly to note::FlashString at the call site:
float t = body.get_float(F("temperature"), 0.0f);
int32_t h = body.get_int  (F("humidity"),    0);

// Or declare reusable flash strings portably:
static const char k_temp[]     NOTE_FLASH_ATTR = "temperature";
static const char k_humidity[] NOTE_FLASH_ATTR = "humidity";

float t2 = body.get_float(note::flash(k_temp),     0.0f);
int32_t h2 = body.get_int (note::flash(k_humidity), 0);
```

You never have to switch — **plain string literals keep working**
via the RAM overload, and the two styles can be mixed freely:

```cpp
// Also valid — the literal lives in RAM, compare is a plain byte match:
float t = body.get_float("temperature", 0.0f);

// Runtime-generated keys go through the RAM path automatically:
char key[24];
snprintf(key, sizeof(key), "sensor_%u", idx);
float v = body.get_float(key, 0.0f);
```

###### When do I need `note::flash()`?

| How you pass the key | What happens | Wrapper needed? |
|---|---|---|
| `"literal"` | `string_view` overload (RAM compare) | — (safe, unoptimized) |
| `char buf[N]` (runtime) | `string_view` overload (RAM compare) | — |
| `F("literal")` | Implicit conversion → `FlashString` overload (`pgm_read_byte`) | **No** |
| `static const char k[] NOTE_FLASH_ATTR = "..."` | Would pick `string_view` overload → reads wrong bytes on AVR | **Yes** — wrap as `note::flash(k)` |
| `note::flash(F("..."))` | Same as `F()` direct, just more typing | Works, but redundant |

**Summary rule:** wrap `NOTE_FLASH_ATTR`-declared arrays with
`note::flash(...)` to route them to the flash overload. Everywhere
else the right overload is picked automatically.

The flash-key overloads exist on both `note::JsonView` and
`note::scan::*`. On non-AVR Arduino cores (ESP32, SAMD, RP2040,
etc.) flash keys compile to a plain pointer load — zero runtime
overhead, no benefit either. Measured on the comparison sketch
(ATmega328P, 5 scan keys, `F()` form): **−16 B RAM, −32 B flash**
vs the RAM-key baseline. Scan keys that are also used as JsonBuf
request keys stay in `.data` regardless — only scan-exclusive
keys move to flash.

> Footgun: never call `note::flash("bare literal")`. A literal
> without `F()` or `NOTE_FLASH_ATTR` lives in RAM, but `note::flash()`
> tells the scanner to read it from flash. On AVR the scanner then
> reads garbage from flash at the RAM-side address. `F()` and
> `NOTE_FLASH_ATTR` are safe by construction.

For many fields, populate a struct in a single pass via
`NOTE_FIELDS`:

```cpp
struct Readings {
    float temp;
    int32_t humidity;
    bool alarm;
    NOTE_FIELDS(temp, humidity, alarm)
};

Readings r{};
note::JsonView(*resp).object("body").into(r);
```

> **Caveat:** `JsonView` is a substring scanner, not a full JSON
> parser. It does not decode string escape sequences (`\n`, `\"`,
> `\uXXXX`) and is not safe against adversarial input. Use SAX when
> you need robust parsing.

See `tools/binary-size-comparison/` in the repo for the full
comparison sources (environments `avr-notecpp-raw` for SAX and
`avr-notecpp-scan` for `JsonView`).

See [feature flags](../../feature-flags.md) for all compile-time options.
