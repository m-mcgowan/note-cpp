# note-cpp

[![CI](https://github.com/m-mcgowan/note-cpp/actions/workflows/ci.yml/badge.svg)](https://github.com/m-mcgowan/note-cpp/actions/workflows/ci.yml)
[![codecov](https://codecov.io/gh/m-mcgowan/note-cpp/graph/badge.svg?token=9JJP6N9QAE)](https://codecov.io/gh/m-mcgowan/note-cpp)
![C++ Standard](https://img.shields.io/badge/C%2B%2B-17%20%7C%2020%20%7C%2023-blue)
![Header Only](https://img.shields.io/badge/header--only-yes-green)
[![License: MIT](https://img.shields.io/badge/license-MIT-yellow)](LICENSE)

Type-safe C++ API for the [Blues Notecard](https://blues.com/notecard). Header-only, zero dependencies beyond the standard library. C++17, C++20, and C++23 — each unlocks additional features.

> **Community project.** Not affiliated with or supported by Blues Inc. Notecard is a trademark of Blues Inc. Familiarity with the [Notecard](https://blues.com/blog/getting-started-with-the-notecard/) and its [API](https://dev.blues.io/api-reference/notecard-api/introduction/) is assumed.

## Quick start

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

Install from GitHub: **Sketch → Include Library → Add .ZIP Library** and point to this repository's ZIP download.

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

The typed API is the same on every platform. As an alternative to the fluent style shown above, you can also use assignment to set request properties.

<!-- snippet:direct-assignment examples/arduino/readme_snippets/readme_snippets.ino:47-51 -->
```cpp
auto req = nc.hub.set();
req.product = "com.example.app";
req.mode = "periodic";
req.outbound = 60_mins;
req.execute();
```

Your own custom structs allow you to send and receive arbitrary bodies (say, with `note.add`). The same struct is used for sending and receiving notes, as well as for `note.template` registration.

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

Custom structs can also be used for environment variables. (TODO link to the example.)

Responses carry typed fields and a truthy operator:

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

TODO - shouldn't the full walkthrough be a document rather than a pointer to code?
Full walkthrough: [examples/stdcpp/getting-started.cpp](examples/stdcpp/getting-started.cpp). If you are migrating from note-arduino / note-c, the [migration guide](docs/platforms/arduino/migration-from-note-arduino.md) has side-by-side examples.

## What's in the library

TODO - do not use the word endpoint - it's not a term that Blues use so will be unfamiliar to readers. (I am sure this is noted in memory.)
TODO - let's mention that it can work with zero heap.

- **[Typed API](docs/using-the-api.md)** — typed requests and responses for all 74 Notecard APIs; fluent or direct-assignment styles; on C++20 the compiler validates string constants for fields like `mode` at compile time. (TODO - link to the docs that show field validation.)
- **[Focused operations](docs/using-the-api.md#focused-operations-on-multi-purpose-endpoints)** — distinct types per intent for multi-purpose requests (`note.get` vs `note.pop`, `card.location.mode.fixed()` vs `.get()`); setting a non-applicable field is a compile error.
- **[Body values and Note templates](docs/body-values.md)** — one struct for send, receive, and template registration; plain aggregates work directly on C++20+, with `NOTE_FIELDS(...)` available on C++17 or for non-aggregates.
- **[Duration units](docs/duration-units.md)** — `Minutes`, `Seconds`, `Hours`, `Days` with safe implicit conversion to smaller units; wrong direction is a compile error.
- **[Error handling](docs/error-handling.md)** — truthy responses on success, structured `ErrorInfo` on failure; per-request safety classification (`ReadOnly`, `Idempotent`, `NonIdempotent`, `Destructive`) informs retry.
- **[Target filtering](docs/feature-flags.md#target-filtering-c20) (C++20)** — constrain the available APIs by hardware variant (WiFi/Cell/Skylo/LoRa) and/or minimum firmware version; unsupported endpoints become compiler warnings, or errors in strict mode.
- **[Streaming and tree modes](docs/streaming-and-tree.md)** — two JSON-parsing strategies; tree mode keeps a walkable `JsonReader` on the response, while streaming mode reads the wire directly into your typed struct with no tree in memory.
- **[Memory control](docs/memory.md)** — you choose where response strings live: a static `MonotonicArena` (zero heap, bounded RAM, predictable on every target), a `HeapResetPool` (malloc-backed with arena lifecycle), default `malloc`/`free`, `std::pmr`, or a custom function-pointer `Allocator`. The same surface across all five — swap the allocator, the typed API does not change.
- **[JSONB wire format](docs/jsonb.md)** — optional `NOTE_JSONB` swaps JSON text for compact binary opcodes; reduces flash on constrained targets. (Enabled automatically on constrained targets.)
- **Wire protocols** — Implements the expected wire protocol for Notecard. header-only [serial (`SerialFramer`)](docs/transport-serial.md) and [I2C (`I2cFramer`)](docs/transport-i2c.md) with CRC auto-detection, segmented TX/RX, retry, auto-reset. Binary transfer (`card.binary.put`/`get`) uses COBS framing internally.

[C++ version compatibility matrix](./docs/cpp-version-compatibility.md) — what's available on C++17, what unlocks on C++20/23.

## How it scales

The library is built to scale from resources-constrained MCUs to desktop-class hardware. The same API surface can be used from ATmega328P (32 KB flash / 2 KB RAM) up to ESP32, Cortex-M, and desktop hosts. For additional optimization, you dial resource use by choosing how much of the stack to pull in.

### Target tiers

| Target | Defaults | Recommended flags | Typical flash / RAM |
|---|---|---|---|
| **AVR Uno** (ATmega328P) | streaming, zero heap | `NOTE_MINIMAL` (auto-enables `NOTE_JSONB`), `JsonView` / `note::scan` for responses | 10.9 – 24.3 KB / 680 – 836 B |
| **Cortex-M0 / STM32** | streaming, zero heap | `NOTE_MINIMAL` | typed API fits comfortably |
| **ESP32 / Cortex-M4+** | streaming with arena allocator | defaults | full typed API + body structs |
| **Linux / macOS host** | tree path with a JSON backend | `cJSON` or `nlohmann` backend | full surface, heap allowed |

### The full progression (Arduino Uno, 8-endpoint app)

Each row peels off one more layer; the typed API (rows 1–2) is what most users want, rows 3–5 trade some convenience for reduced flash use. All five styles share the same transport and can be mixed in one image — the compiler drops what you don't call.

| # | Style | Flash | Δ flash vs typed | RAM |
|---|---|---|---|---|
| — | **note-c** baseline (`Notecard::requestAndResponse`) | 25,076 B | +2 B | 729 B + 371 B heap |
| 1 | **Typed API groups** (`api.hub.set().product(...).execute()`) | 25,470 B | +396 B | 804 B + 0 B heap |
| 2 | **Typed direct** (`nc.execute(HubSet{...})`) | 25,074 B | baseline | 768 B + 0 B heap |
| 3 | **[Raw JSON + SAX sink](docs/using-the-api.md#raw-json)** ([`JsonBuf`](docs/json-builder.md) + `transact_dispatch` + `JsonSink`) | 21,192 B | −3,882 B | 792 B + 0 B heap |
| 4 | **[Raw + `JsonView` scan](docs/using-the-api.md#raw-json)** (RAM keys) | 11,110 B | **−13,964 B** | 696 B + 0 B heap |
| 5 | **[Raw + `JsonView` scan](docs/using-the-api.md#raw-json)** (`F()` flash keys) | **11,078 B** | **−13,996 B** | **680 B** + 0 bytes heap |

Per-row code patterns: [Arduino guide § Binary size comparison](docs/platforms/arduino/guide.md#binary-size-comparison). Compile-time switches: [docs/feature-flags.md](docs/feature-flags.md). Benchmark harness: [tools/binary-size-comparison/](tools/binary-size-comparison/).

## Documentation

`note-cpp` ships full prose documentation alongside the headers. The four pointers below cover the most common entry points; [`docs/README.md`](docs/README.md) is the full index.

- **Start here:** [Getting started](docs/getting-started.md) — top-down walkthrough from a clean project to your first request
- [Migrating from note-arduino](docs/platforms/arduino/migration-from-note-arduino.md) — side-by-side examples for common patterns
- [Feature flags](docs/feature-flags.md) — compile-time options for binary size optimization (AVR, Cortex-M0)
- [Full documentation index](docs/README.md) — all guides, from getting started to internals
- API reference (Doxygen) — generate locally with `./ci.sh --docs`

## Quality assurance

Host coverage **97.5% lines / 99.0% functions / 96.2% branches**; on-device coverage (ESP32-S3) **81.6% lines / 82.7% functions**. The same `TEST_CASE`s run on the host doctest binaries (under 5 compilers) and on real Notecard hardware over serial/I2C, plus a Wokwi-simulated AVR runtime to catch Uno-specific init/stack issues that static build verification can't. Docs are verified pre-push: every internal link resolves, every code snippet comes from a compiled source file, and the migration tables stay column-aligned by tooling.

Full breakdown: [docs/quality-assurance.md](docs/quality-assurance.md).

## Contributing

Bug reports and PRs welcome via [Issues](https://github.com/m-mcgowan/note-cpp/issues) and [Pull Requests](https://github.com/m-mcgowan/note-cpp/pulls). See [CONTRIBUTING.md](CONTRIBUTING.md) for development setup, building, testing, and codegen.
