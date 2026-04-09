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

### Diagnostics

| Flag | Default | Minimal | Effect | Savings |
|------|---------|---------|--------|---------|
| `NOTE_DEBUG_ENABLED` | `1` | **(M)** `0` | Enable runtime debug listener support (`set_debug()`). When `0`, all debug calls compile to no-ops and the `DebugListener` field is eliminated. | ~16 B RAM, ~500 B flash |
| `NOTE_SHORT_ERRORS` | `0` | **(M)** `1` | Collapse all `NOTE_ERR("message")` strings to `"E"`. On AVR (Harvard architecture), this saves RAM because string literals are copied from flash to RAM at startup. | ~350 B RAM |

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

Result: ~20 KB flash (63%), ~736 B RAM (36%), zero heap.

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

## Where flags are defined

- **`include/note/note_config.hpp`** — `NOTE_MINIMAL` defaults
- **`include/note/compiler.hpp`** — `NOTE_SHORT_ERRORS`, `NOTE_ERR()` macro
- **`include/note/error.hpp`** — `NOTE_PRINTABLE` default
- **`include/note/lexer/json_lexer.hpp`** — `NOTE_UNICODE_ESCAPES` → `BasicEscapeDecoder`
- **`include/note/static_notecard.hpp`** — `NOTE_NO_RETRY`, `NOTE_NO_REQUEST_IDS`
- **`include/note/streaming_transport.hpp`** — `NOTE_NO_CRC`, `NOTE_MINIMAL` (lookahead), `NOTE_DEBUG_ENABLED`

## Size impact summary (AVR ATmega328P, full body example)

| Configuration | Flash | Static RAM | Heap (peak) | Total RAM |
|--------------|-------|------------|-------------|-----------|
| `NOTE_MINIMAL` | 20,392 (63%) | 736 (36%) | 0 (0%) | 736 (36%) |
| No flags (full) | ~31,000+ | ~2,600+ | not measured | — |
| note-c (reference) | 24,646 (76%) | 739 (36%) | 371 (18%) | 1,110 (54%) |

note-c heap measured via `__brkval` watermark on Wokwi (8-endpoint app with mock Notecard).
