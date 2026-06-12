# Feature flags

`note-cpp` uses compile-time feature flags to customize the library for your platform. Flags fall into two categories:

- **Flash/memory optimization** — trade features for smaller binary size on constrained devices (AVR, Cortex-M0)
- **Developer convenience** — control namespaces, API style, debug output

All flags are optional. The defaults produce a full-featured build suitable
for ESP32, STM32, and other 32-bit platforms. The library is already
efficient for typical microcontrollers — its streaming architecture means
zero heap allocations and low RAM overhead by default. The optimization
flags below enable additional flash savings when targeting the most
constrained devices.

**All flags should be defined to `1` or `0`, not bare `#define`.** For
example, use `-DNOTE_MINIMAL=1` or `#define NOTE_MINIMAL 1`, not
`#define NOTE_MINIMAL` with no value. When using `-D` on the command
line without a value (e.g. `-DNOTE_MINIMAL`), most compilers default to
`1`, so command-line usage is fine. The explicit `=1`/`=0` convention
matters when defining flags in source code.

## Flash-size reduction with `NOTE_MINIMAL`

For constrained platforms (AVR, Cortex-M0), define `NOTE_MINIMAL` to
set all size-saving defaults at once. This trades some features (CRC, retry, tree path, ad-hoc fields) for a significantly smaller binary:

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
| `NOTE_JSONB` | `0` | **(M)** `1` | Use [JSONB binary wire format](jsonb.md) instead of JSON text. Requests/responses encoded as COBS-framed binary opcodes. CRC is bypassed (COBS provides framing but not integrity — CRC handles that separately). Raw JSON string bodies are a compile error — use lambdas or typed structs. Requires Notecard firmware 11.x+. | ~1.9 KB flash (replaces JSON builder/lexer with the smaller JSONB builder/parser) |
| `NOTE_NO_JSON_TREE` | off | **(M)** on | Disable `JsonBackend`/`JsonReader` tree-mode parse path (`.body()`, `.body_or_error()`, `parse(reader)`). Only streaming SAX parse available. The legacy spelling `NOTE_NO_BUFFERED` is honoured as a deprecated alias. | ~2-4 KB flash, ~300 B RAM |
| `NOTE_I2C_BUS_LOCK` | `1` | **(M)** `0` | Enable the optional bus-lock hook on the I2C transport. When a lock is registered via `transport.set_bus_lock()`, each wire exchange is bracketed by `IBusLock::lock()`/`unlock()`. When `0`, the hook code is fully compiled out. See [Sharing the bus / multi-threaded use](transport-i2c.md#sharing-the-bus--multi-threaded-use). | One pointer + null check per exchange on the polymorphic path; zero cost on the static `NullLock` template path |
| `NOTE_NO_CRC` | off | **(M)** on | Disable CRC32 on request/response framing. (CRC not supported by JSONB on Notecard)| ~200 B flash, 64 B .data (LUT) |
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
| `NOTE_USING_NAMESPACE` | `1` | Import `using namespace note` from `note.hpp`. Set to `0` to disable all imports. |
| `NOTE_USING_LITERALS` | `1` | Import `using namespace note::literals` (`15_mins`, `5_s`, `7_days`). |
| `NOTE_USING_ATTN` | `1` | Import `using namespace note::attn` (flag constants: `connected`, `motion`, etc.). |
| `NOTE_USING_SERIAL` | `1` | Import `using namespace note::serial` (serial mode constants). |
| `NOTE_USING_TRIANGULATE` | `1` | Import `using namespace note::triangulate` (triangulation mode constants). |

`note.hpp` imports the `note` namespace and sub-namespaces by default so user
code reads naturally. On Arduino, `Notecard` is also imported.

Sub-flags default to the value of `NOTE_USING_NAMESPACE`, so setting the
master to `0` disables everything. Override individual flags to re-enable
selectively:

```ini
# Disable all namespace imports
build_flags = -DNOTE_USING_NAMESPACE=0

# Disable all, but keep duration literals
build_flags = -DNOTE_USING_NAMESPACE=0 -DNOTE_USING_LITERALS=1

# Keep everything except attn constants
build_flags = -DNOTE_USING_ATTN=0
```

### API version gating and strict mode

| Flag | Default | Effect |
|------|---------|--------|
| `NOTE_API_VERSION` | Latest (currently `NOTE_VERSION(9, 1, 1)`) | Target firmware version. Fields added after this version produce `[[deprecated]]` warnings. |
| `NOTE_API_STRICT` | off | Remove version-gated fields entirely (compile error instead of warning). |

`NOTE_API_VERSION` gates **individual fields** based on the firmware version that introduced them (the spec's `x-min-api-version`). Without `NOTE_API_STRICT`, newer fields warn but stay callable; with strict mode, they're compiled out and using one is a compile error.

```ini
# Warn about fields newer than firmware 7.2.1
build_flags = -DNOTE_API_VERSION=NOTE_VERSION(7,2,1)

# Strict: error on fields newer than 7.2.1
build_flags = -DNOTE_API_VERSION=NOTE_VERSION(7,2,1) -DNOTE_API_STRICT
```

For **endpoint-level** filtering (entire endpoints, not individual fields), use the C++20 target filtering API — see below.

### Target filtering (C++20)

C++20 concepts let you constrain the `Api` by hardware variant and/or minimum firmware version. Better diagnostics than the preprocessor flags above; same opt-in to strict mode (warning becomes compile error).

```cpp
Api cell_api(nc, target<Hardware::Cell>());           // hardware
Api old_api(nc, min_firmware<5, 0, 0>());             // firmware
Api both_api(nc, target<Hardware::WiFi, 9, 1, 1>());  // both
auto strict = target<Hardware::LoRa>().as_strict();   // strict variant
```

Full walkthrough with all variants: [examples/target-filtering.cpp](../examples/stdcpp/target-filtering.cpp).

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

The Api convenience groups (`api.hub.set().execute()`) add a small flash overhead for the factory structs and group wiring — negligible on 32-bit platforms, measurable on AVR Uno. The direct-assignment style (`nc.execute(req)` with a plain request struct) avoids it entirely. Use `-DAPI_STYLE=2` in the binary-size benchmark to see the delta; the full per-style writeup is in [using-the-api.md § Calling styles](using-the-api.md#calling-styles-within-the-typed-layer).

## Size impact summary (AVR ATmega328P, 8-endpoint app with body parsing)

| Configuration | Flash | Static RAM | Heap (peak) | Total RAM |
|--------------|-------|------------|-------------|-----------|
| `note-cpp` `NOTE_MINIMAL` (JSONB) | 24,290 (75%) | 832 (41%) | 0 (0%) | 832 (41%) |
| `note-cpp` `NOTE_MINIMAL` + `NOTE_JSONB=0` (JSON) | 26,484 (82%) | 832 (41%) | 0 (0%) | 832 (41%) |
| `note-c` (reference) | 25,076 (78%) | 729 (36%) | 371 (18%) | 1,100 (54%) |

With JSONB (the default for `NOTE_MINIMAL`), `note-cpp` is **786 bytes smaller**
in flash than `note-c` while using 24% less total RAM (zero heap vs 371 bytes
peak). The JSON text path is 1,408 bytes larger in flash due to the streaming
SAX infrastructure, but this gap is eliminated by JSONB's simpler binary
encoding.

Applications that know their response shapes ahead of time can skip the SAX
machinery entirely by using `note::JsonView` / `note::scan::*` with
`transact_raw`. This saves another ~8 KB of flash on top of the numbers above,
landing at **10,882 bytes flash / 680 B RAM** on the same 8-endpoint sketch —
see the [Arduino guide](platforms/arduino/guide.md#binary-size-comparison)
for the full progression and the code patterns for each style.

note-c heap measured via `__brkval` watermark on Wokwi (mock Notecard).
