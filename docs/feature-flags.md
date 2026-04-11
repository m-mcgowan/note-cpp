# Feature Flags

`note-cpp` uses compile-time feature flags to control binary size and
feature availability. All flags are optional — the defaults produce a
full-featured build suitable for ESP32, STM32, and other 32-bit platforms.

## Quick start: `NOTE_MINIMAL`

For constrained platforms (AVR, Cortex-M0), define `NOTE_MINIMAL` to
set all size-saving defaults at once:

```ini
# platformio.ini
build_flags = -DNOTE_MINIMAL
```

`NOTE_MINIMAL` is equivalent to defining all of the flags in the table
below marked with **(M)**. Individual flags can still be overridden:

```ini
# Minimal, but keep CRC support
build_flags = -DNOTE_MINIMAL -UNDEF_NOTE_NO_CRC
```

## Flag reference

### Transport and protocol

| Flag | Default | Minimal | Effect | Savings |
|------|---------|---------|--------|---------|
| `NOTE_NO_BUFFERED` | off | **(M)** on | Disable `JsonBackend`/`JsonReader` buffered parse path. Only streaming SAX parse available. | ~2-4 KB flash, ~300 B RAM |
| `NOTE_NO_CRC` | off | **(M)** on | Disable CRC32 on request/response framing. | ~200 B flash, 64 B .data (LUT) |
| `NOTE_NO_MD5` | off | **(M)** on | Disable MD5 for binary transfer verification. | ~512 B .data (tables) |
| `NOTE_NO_STD_STRING` | off | **(M)** on | Disable `std::string`-dependent features (debug wire tracing, some transport methods). Required for AVR (no `<string>` header). | Enables AVR builds |
| `NOTE_NO_RETRY` | off | **(M)** on | Disable safety-gated retry and inter-transaction timing. Single-attempt transactions only. | ~8 B RAM (RetryPolicy + TransactionTiming fields) |
| `NOTE_NO_REQUEST_IDS` | off | **(M)** on | Disable automatic request ID generation (`"id"` field in requests). | ~5 B RAM (counter + bool) |

### Response parsing and API surface

| Flag | Default | Minimal | Effect | Savings |
|------|---------|---------|--------|---------|
| `NOTE_EXTRAS` | `1` | **(M)** `0` | Disable `.extra()` and `operator[]` for ad-hoc fields on requests. | ~100-200 B flash per endpoint |
| `NOTE_PRINTABLE` | `1` (Arduino) / `0` (other) | **(M)** `0` | Disable Arduino `Printable` support on `ErrorInfo`, `ResponseField`, and `Response` types. | ~900 B flash (vtable entries) |
| `NOTE_UNICODE_ESCAPES` | on | **(M)** off | Enable `\uXXXX` JSON escape decoding. Without this, the lexer only handles basic escapes (`\n`, `\t`, `\\`, `\"`, etc.). Notecard responses never use unicode escapes. | ~234 B flash |
| `NOTE_NO_API_GROUPS` | off | off | Prevent instantiation of `Api` convenience groups. Any `note::Api<NcT>` triggers a compile error pointing to direct assignment. See [API styles](#api-styles-and-flash-cost). | ~500 B flash |

### Diagnostics

| Flag | Default | Minimal | Effect | Savings |
|------|---------|---------|--------|---------|
| `NOTE_DEBUG_ENABLED` | `1` | **(M)** `0` | Enable runtime debug listener support (`set_debug()`). When `0`, all debug calls compile to no-ops and the `DebugListener` field is eliminated. | ~16 B RAM, ~500 B flash |
| `NOTE_SHORT_ERRORS` | `0` | **(M)** `1` | Collapse all `NOTE_ERR("message")` strings to `"E"`. On AVR (Harvard architecture), this saves RAM because string literals are copied from flash to RAM at startup. | ~350 B RAM |

### Namespace imports

| Flag | Default | Effect |
|------|---------|--------|
| `NOTE_NO_USING_NAMESPACE` | off | Disable all global namespace imports from `note.hpp` |
| `NOTE_NO_USING_LITERALS` | off | Disable only `using namespace note::literals` (keep `Notecard` etc.) |

On Arduino, `note.hpp` imports `Notecard` and duration literals (`15_mins`, `5_s`, etc.) into the global namespace for developer convenience. Define these flags before `#include <note.hpp>` if you need to avoid name collisions.

## Overriding `NOTE_MINIMAL` defaults

Every flag set by `NOTE_MINIMAL` uses `#ifndef` — if you define the flag
before `note_config.hpp` is included, your value wins:

```ini
# Minimal build, but keep retry support and debug
build_flags =
    -DNOTE_MINIMAL
    -DNOTE_DEBUG_ENABLED=1
    -UNOTE_NO_RETRY
```

You can also re-enable unicode escapes:

```ini
build_flags = -DNOTE_MINIMAL -DNOTE_UNICODE_ESCAPES
```

## Typical configurations

### Arduino Uno (AVR ATmega328P) — 32 KB flash, 2 KB RAM

```ini
build_flags = -DNOTE_MINIMAL
```

Result: ~27.5 KB flash (85%), ~832 B RAM (41%), zero heap (8-endpoint app
with body parsing). See [API styles](#api-styles-and-flash-cost) below.

### ESP32 / STM32 / RP2040 — no constraints

No flags needed — defaults are full-featured.

### ESP32 with reduced binary

```ini
build_flags = -DNOTE_EXTRAS=0 -DNOTE_PRINTABLE=0
```

Disables ad-hoc extras and Arduino Printable for a smaller binary while
keeping all protocol features.

### Production firmware — no debug, no extras

```ini
build_flags = -DNOTE_DEBUG_ENABLED=0 -DNOTE_EXTRAS=0
```

## API styles and flash cost

`note-cpp` offers two usage styles for building requests:

**Convenience groups** (`Api` factory):
```cpp
note::Api api(nc);
api.hub.set().product("com.example").mode("periodic").execute();
auto temp = api.card.temp().read().execute();
```

**Direct assignment**:
```cpp
note::api::HubSet req;
req.product = "com.example";
req.mode = "periodic";
nc.execute(req);
```

Both styles produce the same wire format and compile to the same execute path.
The convenience groups add a small amount of flash overhead for the factory
structs and group wiring, which is typically negligible on 32-bit platforms.

On very constrained targets (AVR Uno), the direct assignment style avoids
this overhead entirely. Use `-DAPI_STYLE=2` in the binary size comparison
example to see the difference.

## Where flags are defined

- **`include/note/note_config.hpp`** — `NOTE_MINIMAL` defaults
- **`include/note/compiler.hpp`** — `NOTE_SHORT_ERRORS`, `NOTE_ERR()` macro
- **`include/note/error.hpp`** — `NOTE_PRINTABLE` default
- **`include/note/lexer/json_lexer.hpp`** — `NOTE_UNICODE_ESCAPES` → `BasicEscapeDecoder`
- **`include/note/static_notecard.hpp`** — `NOTE_NO_RETRY`, `NOTE_NO_REQUEST_IDS`
- **`include/note/streaming_transport.hpp`** — `NOTE_NO_CRC`, `NOTE_MINIMAL` (lookahead), `NOTE_DEBUG_ENABLED`

## Size impact summary (AVR ATmega328P, 8-endpoint app with body parsing)

| Configuration | Flash | Static RAM | Heap (peak) | Total RAM |
|--------------|-------|------------|-------------|-----------|
| `note-cpp` `NOTE_MINIMAL` | 27,498 (85%) | 832 (41%) | 0 (0%) | 832 (41%) |
| `note-c` (reference) | 25,076 (78%) | 729 (36%) | 371 (18%) | 1,100 (54%) |

`note-cpp` uses 24% less total RAM than `note-c` (zero heap vs 371 bytes
peak heap allocation). The flash gap (2,422 bytes) is the cost of streaming
SAX response parsing — the shared infrastructure that enables zero-copy,
zero-heap operation.

note-c heap measured via `__brkval` watermark on Wokwi (mock Notecard).

The flash gap is shared infrastructure that does not grow with the number
of endpoints — adding more endpoints costs only field descriptor tables
(~30 bytes each).
