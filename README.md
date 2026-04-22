# note-cpp

[![CI](https://github.com/m-mcgowan/note-cpp/actions/workflows/ci.yml/badge.svg)](https://github.com/m-mcgowan/note-cpp/actions/workflows/ci.yml)
[![codecov](https://codecov.io/gh/m-mcgowan/note-cpp/graph/badge.svg?token=9JJP6N9QAE)](https://codecov.io/gh/m-mcgowan/note-cpp)
![C++ Standard](https://img.shields.io/badge/C%2B%2B-17%20%7C%2020%20%7C%2023-blue)
![Header Only](https://img.shields.io/badge/header--only-yes-green)
[![License: MIT](https://img.shields.io/badge/license-MIT-yellow)](LICENSE)

Type-safe C++ API for the [Blues Notecard](https://blues.com/notecard). Header-only, zero dependencies (just the standard library). Works with C++17, C++20, and C++23 — each version unlocks additional features. Compatibility tested on the most popular embedded platforms using PlatformIO.

> **Community project.** Not affiliated with or supported by Blues Inc. Notecard is a trademark of Blues Inc.

## Quick Start

This section assumes you are familiar with the [Blues Notecard](https://blues.com/blog/getting-started-with-the-notecard/) and its [API](https://dev.blues.io/api-reference/notecard-api/introduction/).

### Arduino

<!-- snippet:arduino-quickstart examples/arduino/readme_snippets/readme_snippets.ino:13-13 -->
<!-- snippet:arduino-declare examples/arduino/readme_snippets/readme_snippets.ino:25-25 -->
<!-- snippet:arduino-setup examples/arduino/readme_snippets/readme_snippets.ino:30-35 -->
```cpp
#include <note.hpp>

Notecard nc;

nc.begin(Serial1, 9600);       // serial — or nc.begin(Wire) for I2C

nc.hub.set()
    .product("com.example.app")
    .mode("periodic")
    .execute();
```

Install from GitHub: **Sketch → Include Library → Add .ZIP Library** and point to this repository's ZIP download, or add `https://github.com/m-mcgowan/note-cpp.git` as a library dependency in PlatformIO.

### PlatformIO

```ini
lib_deps = https://github.com/m-mcgowan/note-cpp.git
```

### CMake

```cmake
add_subdirectory(note-cpp)
target_link_libraries(my_app PRIVATE note-cpp)
```

## Examples

Once set up, the typed API is the same on every platform:

<!-- snippet:fluent-api examples/arduino/readme_snippets/readme_snippets.ino:39-43 -->
```cpp
nc.hub.set()
   .product("com.example.app")
   .mode("periodic")
   .outbound(60_mins)
   .execute();
```

Or use direct assignment:

<!-- snippet:direct-assignment examples/arduino/readme_snippets/readme_snippets.ino:47-51 -->
```cpp
auto req = nc.hub.set();
req.product = "com.example.app";
req.mode = "periodic";
req.outbound = 60_mins;
req.execute();
```

Supports sending type-safe notes with body structs — define once, use for send, receive, and template registration:

<!-- snippet:body-struct-def examples/arduino/readme_snippets/readme_snippets.ino:17-21 -->
<!-- snippet:body-send examples/arduino/readme_snippets/readme_snippets.ino:55-59 -->
```cpp
struct Readings {
    float temperature;
    int16_t humidity;
    NOTE_FIELDS(temperature, humidity)  // optional on C++20
};

Readings readings{.temperature = 22.5f, .humidity = 60};
nc.note.add()
   .file("sensors.qo")
   .body(readings)
   .execute();
```

Read responses with typed fields:

<!-- snippet:read-response examples/arduino/readme_snippets/readme_snippets.ino:74-80 -->
```cpp
auto rsp = nc.card.version().execute();
if (rsp) {
    Serial.println(rsp.version);
    Serial.println(rsp.device);
} else {
    Serial.println(rsp.error());
}
```

See the [getting started example](examples/stdcpp/getting-started.cpp) for a complete walkthrough.

> **Coming from note-arduino / note-c?** The
> [migration guide](docs/platforms/arduino/migration-from-note-arduino.md) has side-by-side
> examples covering setup, hub.set, note.add, templates, error handling,
> binary transfers, and more to help you migrate to note-cpp.

## Features

<details>
<summary><strong>Strongly-Typed API</strong> — typed requests and responses for all 74 Notecard endpoints</summary>

Every Notecard request and response is strongly-typed with named fields, chainable setters, and an `execute()` method:

```cpp
// Fluent builder
nc.hub.set()
    .product("com.example.app")
    .mode("periodic")
    .outbound(60_mins)
    .execute();

// Direct assignment (equivalent)
auto req = nc.hub.set();
req.product = "com.example.app";
req.mode = "periodic";
req.outbound = 60_mins;
req.execute();
```

Requests also support ad-hoc fields via `operator[]` (`req["custom"] = value`), fire-and-forget commands (`.command()`), and on C++20 the compiler validates enum fields like `mode` at compile time — no runtime surprises from typos.

See [API reference](docs/api-reference.md) for the full endpoint list, and [API patterns](docs/api-patterns.md) for how Notecard request names map to C++ methods.

</details>

<details>
<summary><strong>Intent-Scoped APIs</strong> — distinct types for multi-purpose requests</summary>

Some Notecard requests behave differently depending on which fields you send. In `note-cpp`, each intent is a distinct type with only the fields that apply:

```cpp
// Read a Note by ID
auto rsp = nc.note.read("data.db").noteId("my-note").execute();

// Pop from queue and parse body into a struct
Readings data{};
auto rsp = nc.note.pop("sensors.qi").into(data).execute();

// card.location.mode — fixed() accepts lat/lon, get() doesn't
nc.card.location.mode.fixed()
    .lat(42.565).lon(-70.783)
    .execute();
```

Setting a field that doesn't apply to that operation is a compile error. See [docs/intent-focused-apis.md](docs/intent-focused-apis.md) for the full list and [API reference](docs/api-reference.md) for all endpoints.

</details>

<details>
<summary><strong>Body Values and Note Templates</strong> — one struct for send, receive, and template registration</summary>

Define a body struct once and use it everywhere. On C++20+, plain aggregates work automatically. On C++17, or for non-aggregate structs (e.g. with constructors), add `NOTE_FIELDS(...)`. See [docs/body-values.md](docs/body-values.md).

<!-- snippet:body-struct-def examples/arduino/readme_snippets/readme_snippets.ino:17-21 -->
<!-- snippet:body-send examples/arduino/readme_snippets/readme_snippets.ino:55-59 -->
<!-- snippet:body-receive examples/arduino/readme_snippets/readme_snippets.ino:63-64 -->
<!-- snippet:body-template examples/arduino/readme_snippets/readme_snippets.ino:69-69 -->
```cpp
struct Readings {
    float temperature;
    int16_t humidity;
    NOTE_FIELDS(temperature, humidity)  // optional on C++20
};

Readings readings{.temperature = 22.5f, .humidity = 60};
nc.note.add()
   .file("sensors.qo")
   .body(readings)
   .execute();

Readings data{};
nc.note.pop("sensors.qi").into(data).execute();

nc.note.templates().define("sensors.qo").body(template_of(Readings())).execute();
```

Request bodies can also be set with `json_fmt` (C++20, compile-time validated), builder lambdas, or raw strings. See [docs/body-values.md](docs/body-values.md).

</details>

<details>
<summary><strong>Type-Safe Duration Units</strong> — <code>Minutes</code>, <code>Seconds</code>, <code>Hours</code>, <code>Days</code> with compile-time safety</summary>

Duration fields use distinct types that make it clear what the units are and prevent accidental unit mixing. Larger units implicitly convert to smaller ones — `7_days` where `Minutes` is expected does the math at compile time.

```cpp
using namespace note::literals;

nc.hub.set()
    .outbound(15_mins)
    .inbound(7_days)             // Days → Minutes (10080 on the wire)
    .execute();

nc.card.attn()
    .seconds(5_mins)             // Minutes → Seconds (300 on the wire)
    .execute();

// Wrong direction is a type error:
// nc.hub.set().outbound(300_s);    // error: Seconds ≠ Minutes
```

Also includes voltage-variable sync builders and comma-separated flag fields with named methods. See [docs/duration-units.md](docs/duration-units.md) and [examples/stdcpp/hub-configuration/](examples/stdcpp/hub-configuration/).

</details>

<details>
<summary><strong>Error Handling</strong> — structured errors and retry guidance</summary>

All requests return a response that is truthy on success. On failure, `error()` provides a structured `ErrorInfo` with an error code, cause, and message:

```cpp
auto rsp = nc.card.version().execute();
if (rsp) {
    Serial.println(rsp.version);
} else {
    Serial.println(rsp.error());  // prints "send_failed[timeout]: no response"
}
```

Error details are available for programmatic handling:

```cpp
ErrorInfo err = rsp.error();
err.code;     // Error::ResponseLost, Error::Notecard, etc.
err.cause;    // Cause::Timeout, Cause::HalError, etc.
err.message;  // human-readable description
```

Each request carries a compile-time safety level (`ReadOnly`, `Idempotent`, `NonIdempotent`, `Destructive`) for retry decisions. See [docs/error-handling.md](docs/error-handling.md) for more details.

</details>

<details>
<summary><strong>Target Filtering</strong> — compile-time API availability based on Notecard hardware and firmware (C++20)</summary>

All Notecard APIs are initially available. You can
constrain the APIs by Notecard hardware variant (WiFi/Cellular/Skylo), minimum firmware version, or both.
Request types and fields not supported by the Notecard hardware or firmware produce compiler warnings, or errors in strict mode.

```cpp
// Hardware only — warns if endpoint doesn't support WiFi
Api wifi_api(nc, target<Hardware::WiFi>());

// Firmware only — warns if endpoint requires newer firmware
Api fw_api(nc, min_firmware<9, 1, 1>());

// Both — hardware and firmware checked together
Api my_api(nc, target<Hardware::WiFi, 9, 1, 1>());
```

Hardware targets: `Hardware::WiFi`, `Hardware::Cell`, `Hardware::CellWifi`, `Hardware::LoRa`, `Hardware::Skylo`. Firmware versions are sourced from the Notecard API spec. [Strict mode](docs/feature-flags.md#strict-mode) turns warnings into compile errors. See [examples/stdcpp/target-filtering.cpp](examples/stdcpp/target-filtering.cpp) and [docs/feature-flags.md](docs/feature-flags.md#target-filtering-c20).

</details>

<details>
<summary><strong>Streaming Architecture</strong> — zero-heap operation</summary>

Requests are streamed directly to the Notecard — there is no need for a request buffer in memory. Responses are parsed with a SAX (event-driven) parser that populates struct fields as data arrives from Notecard.

On an Arduino Uno (ATmega328P, 32 KB flash / 2 KB RAM), an application with 8 different requests compiles to a similar size as note-c, and uses less RAM:

| | note-c | `note-cpp` (JSONB) | `note-cpp` (JSON) |
|---|---|---|---|
| **Flash** | 25,076 (78%) | 24,290 (75%) | 26,484 (82%) |
| **Static RAM** | 729 (36%) | 832 (41%) | 832 (41%) |
| **Heap (peak)** | 371 (18%) | 0 (0%) | 0 (0%) |
| **Total RAM** | 1,100 (54%) | 832 (41%) | 832 (41%) |

All memory is statically allocated at compile time using `MonotonicArena` and `StaticNotecard`. Enabled by default on AVR. See [docs/feature-flags.md](docs/feature-flags.md) for the compile-time options that enable this on other platforms.

On constrained targets where the typed API's SAX parser is too big, `note-cpp` exposes progressively lower-level response-parsing paths — including a `JsonView` / `note::scan::*` mode that skips the SAX machinery entirely. On the same 8-endpoint Uno benchmark this drops flash to **10,882 bytes (−14 KB vs note-c)** at 680 B RAM. See [docs/platforms/arduino/guide.md](docs/platforms/arduino/guide.md#binary-size-comparison) for the full progression.

</details>

<details>
<summary><strong>Wire Protocols</strong> — serial and I2C with CRC, retry, and binary transfer</summary>

Header-only implementations of the Notecard serial and I2C protocols: `NotecardSerial` and `NotecardI2c`. These handle CRC auto-detection, segmented TX/RX, retry logic, and auto-reset.

Each protocol implementation uses a thin platform HAL — a lightweight interface for UART or I2C hardware access. See [docs/transport.md](docs/transport.md) for the full HAL interface.

Binary transfer APIs (`card.binary.get`, `card.binary.put`) use COBS framing handled internally by the transport. See [docs/binary-transfer.md](docs/binary-transfer.md).

An optional JSONB binary wire format (`NOTE_JSONB`) replaces JSON text with compact binary opcodes for reduced overhead on numeric-heavy payloads as well as smaller flash footprint on constrained devices. See [docs/jsonb.md](docs/jsonb.md).

</details>

<details>
<summary><strong>Streaming vs Buffered</strong> — zero-heap streaming by default, buffered path for migration</summary>

The typed API (`execute()`, response fields, body structs) works identically on both paths. **Streaming** is the default — requests are serialized directly to the wire, responses are SAX-parsed as bytes arrive. No JSON tree in memory.

The **buffered** path builds a JSON tree in memory using a JSON backend (cJSON, nlohmann, or the built-in `BufferJsonBackend`). Use it when migrating from note-c (keeps the cJSON/lambda builder pattern) or when you need `JsonReader` tree access on responses.

See [docs/transport.md](docs/transport.md) for when to use each, and [docs/json-backend.md](docs/json-backend.md) for backend options.

</details>

### C++ Version Compatibility

The core library works with C++17. Each successive standard unlocks additional features:

| Feature | C++17 | C++20 | C++23 |
|---------|:-----:|:-----:|:-----:|
| **Core** | | | |
| Typed API (request builders, responses, fluent setters) | yes | yes | yes |
| [Ad-hoc requests](docs/raw-requests.md) (`nc.request("hub.set", lambda)`) | yes | yes | yes |
| [Error handling](docs/error-handling.md) | yes | yes | yes |
| [Type-safe duration units](docs/duration-units.md) (`Seconds`, `Minutes`, `Hours`, `Days`) | yes | yes | yes |
| **JSON** | | | |
| [JSON backends](docs/json-backend.md) (cJSON, nlohmann, buffer/jsmn) | yes | yes | yes |
| [SAX streaming parser](docs/api-layers.md) (`JsonSink`) | yes | yes | yes |
| [`JsonBuf` runtime builder](docs/json-builder.md) (no allocations) | yes | yes | yes |
| [`consteval` JSON](docs/json-builder.md) (`note::json<>()`) | — | yes | yes |
| **Body structs** | | | |
| [Body structs](docs/body-values.md) with [`NOTE_FIELDS`](docs/body-values.md) macro | yes | yes | yes |
| [Body structs without macro](docs/body-values.md) (plain aggregates via reflection) | — | yes | yes |
| **Compile-time checks** | | | |
| [`consteval` enum validation](docs/api-patterns.md) (`validatedMode()`) | — | yes | yes |
| [Target filtering](docs/feature-flags.md#target-filtering-c20) (hardware + firmware) | — | yes | yes |
| [Version gating](docs/feature-flags.md#api-version-gating-and-strict-mode) (per-field firmware availability) | yes | yes | yes |
| **Memory** | | | |
| [Arena sizing](docs/arena-sizing.md) — [`MonotonicArena`](docs/arena-sizing.md) + arena allocator | yes | yes | yes |
| [`StringPool`](docs/memory.md) response string interning | yes | yes | yes |
| [Zero-alloc `BufferJsonBackend`](docs/json-backend.md) (jsmn) | yes | yes | yes |
| **Standard library** | | | |
| [`std::expected`](docs/cpp-standard-requirements.md) (native, vs `tl::expected` fallback) | — | — | yes |
| [`std::unreachable`](docs/cpp-standard-requirements.md) (native, vs compiler builtins) | — | — | yes |

## How It Scales

`note-cpp` is built to meet the target where it is — from an
ATmega328P (32 KB flash / 2 KB RAM) up to ESP32, Cortex-M, and
desktop-class hosts — without different APIs or separate libraries.
The same typed API surface compiles everywhere; you dial resource
use by choosing how much of the stack to pull in.

### Target tiers

| Target | Defaults | Recommended flags | Typical flash / RAM |
|---|---|---|---|
| **AVR Uno** (ATmega328P) | streaming, zero heap | `NOTE_MINIMAL` (auto-enables `NOTE_JSONB`), `JsonView` / `note::scan` for responses | 10.9 – 24.3 KB / 680 – 836 B |
| **Cortex-M0 / STM32** | streaming, zero heap | `NOTE_MINIMAL` | typed API fits comfortably |
| **ESP32 / Cortex-M4+** | streaming with arena allocator | defaults | full typed API + body structs |
| **Linux / macOS host** | buffered path with a JSON backend | `cJSON` or `nlohmann` backend | full surface, heap allowed |

### The full progression (Arduino Uno, 8-endpoint app)

Each row peels off one layer of abstraction — showing how much flash
(and RAM) you get back by dropping to a lower-level API. Pick the
highest row that fits your target.

| # | Style | Flash | Δ flash vs typed | RAM |
|---|---|---|---|---|
| — | **note-c** baseline (`Notecard::requestAndResponse`) | 25,076 B | +346 B | 729 B\* |
| 1 | **Typed API groups** (`api.hub.set().product(...).execute()`) | 24,730 B | baseline | 836 B |
| 2 | **Typed direct** (`nc.execute(HubSet{...})`) | 24,520 B | −210 B | 804 B |
| 3 | **Raw JSON + SAX sink** (`JsonBuf` + `transact_dispatch` + `JsonSink`) | 20,528 B | −4,202 B | 848 B |
| 4 | **Raw + `JsonView` scan** (RAM keys) | 10,914 B | **−13,816 B** | 696 B |
| 5 | **Raw + `JsonView` scan** (`F()` flash keys) | **10,882 B** | **−13,848 B** | **680 B** |

\*note-c's RAM excludes a ~371 B heap peak; every `note-cpp` row uses
zero heap.

The typed API (rows 1 – 2) comes with the best developer experience and
comfortably fits targets with ≥ 32 KB flash / ≥ 1 KB RAM. Rows 3 – 5 peel
off progressively more of the library's defaults, trading DX for
footprint. All five styles share the same transport, CRC handling, and
segmented TX/RX — you can mix them in one firmware image, and the
compiler drops what you don't use.

See [`docs/platforms/arduino/guide.md#binary-size-comparison`](docs/platforms/arduino/guide.md#binary-size-comparison)
for full code patterns per row, [`docs/feature-flags.md`](docs/feature-flags.md) for the
complete list of compile-time switches, and [`tools/binary-size-comparison/`](tools/binary-size-comparison/)
for the benchmark harness that produced these numbers.

## Architecture

```
┌────────────────────────────────────────────────┐
│  Your application                              │
├────────────────────────────────────────────────┤
│  Typed API layer              note-cpp         │
│    Generated requests & responses              │
│    Body structs · Note templates · targeting   │
├────────────────────────────────────────────────┤
│  Protocol layer               note-cpp         │
│    Notecard serial & I2C framing               │
│    CRC · retry · segmented TX/RX               │
├────────────────────────────────────────────────┤
│  Platform HAL                 note-cpp         │
│    Arduino · Zephyr · ESP-IDF · Linux          │
│    (built-in, selected by build environment)   │
└────────────────────────────────────────────────┘
```

note-cpp is a single library that includes platform HALs for common targets. The HAL for your platform is selected automatically based on your build environment (Arduino framework, Zephyr, ESP-IDF, or POSIX). Custom HALs are a simple callback interface — see [docs/transport.md](docs/transport.md).

## Testing

| Level | What | Count |
|-------|------|-------|
| **Host unit tests** | Catch2 tests covering all endpoints, transport, SAX parsing, body structs, error handling | ~1,400 test cases |
| **Compile-fail tests** | Verify that invalid API usage doesn't compile (wrong types, invalid flags, bad JSON) | 19 |
| **Arduino build** | Same test suite compiled with `ARDUINO` defined, verifying `Printable` integration | ~1,400 test cases |
| **Code coverage** | GCC 13 + lcov 2.x — lines 97%, functions 99%, branches 96% | CI enforced |
| **Multi-compiler CI** | g++ 12/13/14, clang++ 17/18, C++20 and C++23, libstdc++ and libc++ | 5 configurations |
| **On-device integration** | ESP32-S3 with a real Notecard over serial/I2C — API requests, body parsing, binary transfer, streaming SAX | 36 test cases |
| **AVR build verification** | ATmega328P (Arduino Uno) binary size checks | PlatformIO |
| **Embedded compatibility** | Library examples compiled across ESP32, AVR, STM32 via [compat-check](https://github.com/m-mcgowan/embedded-cpp-compat-check) | CI |

Host tests run in ~35 seconds. The full CI matrix (5 compilers + coverage + embedded compat) runs on every push.

## Documentation

- [Migrating from note-arduino](docs/platforms/arduino/migration-from-note-arduino.md) — side-by-side examples for common patterns
- [Feature flags](docs/feature-flags.md) — compile-time options for binary size optimization (AVR, Cortex-M0)
- [Full documentation index](docs/README.md) — all guides, from getting started to internals
- API reference (Doxygen) — generate locally with `./ci.sh --docs`

## Contributing

Requires C++17 minimum. C++20 enables zero-overhead transport policies
and compile-time validation of enum string fields.

Bug reports, feature requests, and pull requests are welcome via
[GitHub Issues](https://github.com/m-mcgowan/note-cpp/issues) and
[Pull Requests](https://github.com/m-mcgowan/note-cpp/pulls).

See [CONTRIBUTING.md](CONTRIBUTING.md) for development setup, building,
testing, and code generation details.
