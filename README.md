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

See [API reference](docs/api-reference.md) for the full endpoint list, and [Using the API](docs/using-the-api.md#calling-styles-within-the-typed-layer) for the calling-style options and how Notecard request names map to C++ methods.

</details>

<details>
<summary><strong>Focused APIs</strong> — distinct types for multi-purpose requests</summary>

Some Notecard requests behave differently depending on which fields you send (`note.get` reads a Note by default, pops it from a queue with `delete:true`; `card.location.mode` queries, configures periodic GPS, fixes a location, or resets — same wire request). `note-cpp` splits these into named **operations**, each a distinct C++ type that only exposes the fields and response shape that apply.

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

Setting a field that doesn't apply to that operation is a compile error. The same direct-type form is also available for generic code or build configurations that disable API groups:

```cpp
// Direct type, designated init (C++20):
auto rsp = nc.execute(note::api::CardLocationMode::Fixed{.lat = 42.565, .lon = -70.783});

// Direct type, assignment (C++17 or non-aggregate):
note::api::CardLocationMode::Fixed req;
req.lat = 42.565;
req.lon = -70.783;
auto rsp = nc.execute(req);
```

See [Focused operations](docs/using-the-api.md#focused-operations-on-multi-purpose-endpoints) for the broader pattern and [API reference](docs/api-reference.md) for every request with both styles.

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

Request bodies can also be set with [`json_fmt`](docs/body-values.md) (C++20, compile-time validated), builder lambdas, or raw strings. See [docs/body-values.md](docs/body-values.md).

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
<summary><strong>Wire Protocols</strong> — serial and I2C with CRC, retry, and binary transfer</summary>

Header-only implementations of the Notecard serial and I2C protocols: [`SerialFramer`](docs/transport-serial.md) and [`I2cFramer`](docs/transport-i2c.md). These handle CRC auto-detection, segmented TX/RX, retry logic, and auto-reset.

Each protocol implementation uses a thin platform HAL — a lightweight interface for UART or I2C hardware access. See [docs/transport.md](docs/transport.md) for the full HAL interface.

Binary transfer APIs (`card.binary.get`, `card.binary.put`) use COBS framing handled internally by the transport. See [docs/binary-transfer.md](docs/binary-transfer.md).

An optional JSONB binary wire format (`NOTE_JSONB`) replaces JSON text with compact binary opcodes for reduced overhead on numeric-heavy payloads as well as smaller flash footprint on constrained devices. See [docs/jsonb.md](docs/jsonb.md).

</details>

### C++ Version Compatibility

The core library works with C++17. Each successive standard unlocks additional features:

| Feature | C++17 | C++20 | C++23 |
|---------|:-----:|:-----:|:-----:|
| **Core** | | | |
| Typed API (request builders, responses, fluent setters) | yes | yes | yes |
| [Ad-hoc requests](docs/using-the-api.md#escape-hatches) (`nc.request("hub.set", lambda)`) | yes | yes | yes |
| [Error handling](docs/error-handling.md) | yes | yes | yes |
| [Type-safe duration units](docs/duration-units.md) (`Seconds`, `Minutes`, `Hours`, `Days`) | yes | yes | yes |
| **JSON** | | | |
| [JSON backends](docs/json-backend.md) (cJSON, nlohmann, buffer/jsmn) | yes | yes | yes |
| [SAX streaming parser](docs/using-the-api.md#raw-json) (`JsonSink`) | yes | yes | yes |
| [`JsonBuf` runtime builder](docs/json-builder.md) (no allocations) | yes | yes | yes |
| [`consteval` JSON](docs/json-builder.md) (`note::json<>()`) | — | yes | yes |
| **Body structs** | | | |
| [Body structs](docs/body-values.md) with [`NOTE_FIELDS`](docs/body-values.md) macro | yes | yes | yes |
| [Body structs without macro](docs/body-values.md) (plain aggregates via reflection) | — | yes | yes |
| **Compile-time checks** | | | |
| [`consteval` enum validation](docs/using-the-api.md#calling-styles-within-the-typed-layer) (`validatedMode()`) | — | yes | yes |
| [Target filtering](docs/feature-flags.md#target-filtering-c20) (hardware + firmware) | — | yes | yes |
| [Version gating](docs/feature-flags.md#api-version-gating-and-strict-mode) (per-field firmware availability) | yes | yes | yes |
| **Memory** | | | |
| [Arena sizing](docs/arena-sizing.md) — [`MonotonicArena`](docs/arena-sizing.md) + arena allocator | yes | yes | yes |
| [`StringPool`](docs/memory.md) response string interning | yes | yes | yes |
| [Zero-alloc `StaticJsonBackend`](docs/json-backend.md) (jsmn) | yes | yes | yes |
| **Standard library** | | | |
| [`std::expected`](docs/internal/cpp-version-blockers.md) (native, vs `tl::expected` fallback) | — | — | yes |
| [`std::unreachable`](docs/internal/cpp-version-blockers.md) (native, vs compiler builtins) | — | — | yes |

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
| **Linux / macOS host** | tree path with a JSON backend | `cJSON` or `nlohmann` backend | full surface, heap allowed |

### The full progression (Arduino Uno, 8-endpoint app)

Each row peels off one layer of abstraction — showing how much flash
(and RAM) you get back by dropping to a lower-level API. Pick the
highest row that fits your target.

| # | Style | Flash | Δ flash vs typed | RAM |
|---|---|---|---|---|
| — | **note-c** baseline (`Notecard::requestAndResponse`) | 25,076 B | +2 B | 729 B\* |
| 1 | **Typed API groups** (`api.hub.set().product(...).execute()`) | 25,470 B | +396 B | 804 B |
| 2 | **Typed direct** (`nc.execute(HubSet{...})`) | 25,074 B | baseline | 768 B |
| 3 | **[Raw JSON + SAX sink](docs/using-the-api.md#raw-json)** ([`JsonBuf`](docs/json-builder.md) + `transact_dispatch` + `JsonSink`) | 21,192 B | −3,882 B | 792 B |
| 4 | **[Raw + `JsonView` scan](docs/using-the-api.md#raw-json)** (RAM keys) | 11,110 B | **−13,964 B** | 696 B |
| 5 | **[Raw + `JsonView` scan](docs/using-the-api.md#raw-json)** (`F()` flash keys) | **11,078 B** | **−13,996 B** | **680 B** |

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

## Streaming or tree

`note-cpp` has two execution paths:

- **Streaming** — SAX events parse the wire bytes directly into your typed response struct or sink. No JSON tree in memory. Zero heap.
- **Tree** — a `JsonBackend` parses the response into an in-memory `JsonReader` you can query by key after the call returns.

For most code, the choice is invisible. The same `nc.card.version().execute()` returns the same `r.version` either way:

```cpp
auto r = nc.card.version().execute();
if (r) {
    log(r.version);   // identical on both paths
    log(r.device);
}
```

`.into(struct)` for body parsing, body lambdas for request building, and the raw `nc.transact(json, buf)` API all behave identically on both paths.

**The user-visible divergence is post-call body inspection.** Streaming commits at call time — you decide what to do with the body before sending the request, and SAX fires events into your sink as bytes arrive. Tree mode parks a parsed `JsonReader` on the response, so you can query body fields by name *after* the call returns:

```cpp
auto r = nc.note.get("data.qi").execute();

// Tree mode — query the parsed JsonReader by key after the call:
if (r && r.body()) {
    double temp = r.body()->get_double("temperature");
    int    hum  = r.body()->get_int("humidity");
}

// Streaming — r.body() is null. Commit a struct (or JsonSink) up front:
struct Readings { float temperature; int humidity; NOTE_FIELDS(temperature, humidity); };
Readings readings{};
nc.note.get("data.qi").into(readings).execute();
```

If you know the body shape ahead of time, `.into(struct)` is the better idiom in either mode — it's faster, has lower memory cost, and works on the smallest targets.

### Picking a backend

Tree mode requires a `JsonBackend`. Streaming wants none. The wire-up:

```cpp
// Streaming — no backend, zero heap, smallest flash.
note::Notecard nc(transport, note::Allocator{});

// Tree, default — cJSON-backed; heap-allocated nodes, familiar from note-c.
note::backends::CjsonBackend backend;
note::Notecard nc(backend, transport);

// Tree, zero-heap — fixed-size jsmn token view over the response bytes.
note::backends::StaticJsonBackend<512, 64> backend;
note::Notecard nc(backend, transport);

// Tree, zero-heap with a real cJSON node graph — tree backed by an arena.
note::MonotonicArena arena(arena_buf);
note::backends::CjsonArenaBackend backend(arena);
note::Notecard nc(backend, transport);

// Tree, nlohmann/json — only worthwhile if the project already pulls it in.
note::backends::NlohmannBackend backend;
note::Notecard nc(backend, transport);
```

See [docs/json-backend.md](docs/json-backend.md) for the full backend comparison and [docs/transport.md](docs/transport.md) for when streaming vs tree fits a deployment.

## Quality Assurance

note-cpp's tests exercise every code path on the platforms users deploy to, with coverage tracked on both host and embedded targets. The documentation is verified at push time so examples in the docs can't drift from working code.

### Library code

The portable test suite is one set of `.cpp` files compiled into multiple binaries — host doctest binaries that run in CI under five compilers, and an ESP32-S3 firmware that flashes onto a real Notecard. The same TEST_CASE — same assertions, same fixtures — runs on both. Backend-specific paths (cJSON, nlohmann/json, the buffer/SAX backend) are exercised on both targets so behaviour stays in lock-step.

| Level | What | Count |
|-------|------|-------|
| **Host unit tests** | doctest tests covering all endpoints, transport, SAX parsing, body structs, error handling | ~1,872 test cases |
| **Arduino host build** | Same sources compiled with `ARDUINO` + stubs, verifying `Printable` integration | ~1,889 test cases |
| **Backend parity** | cJSON / nlohmann / buffer JSON backends run on host and device from one source | 89 test cases |
| **On-device firmware** | ESP32-S3 with a real Notecard over serial/I2C — runs the portable suite plus fixture tests for live API requests, binary transfer, streaming SAX | portable + device-only fixture tests |
| **Wokwi runtime (AVR)** | Examples run on a simulated ATmega328P via `wokwi-cli` (CI) and the VS Code Wokwi extension (local) — catches stack-overflow / Arduino-init failures that static build verification can't | per-example smoke tests |
| **Compile-fail tests** | Verify that invalid API usage doesn't compile (wrong types, invalid flags, bad JSON) | 19 |
| **Multi-compiler CI** | g++ 12/13/14, clang++ 17/18, C++20 and C++23, libstdc++ and libc++ | 5 configurations |
| **AVR build verification** | ATmega328P (Arduino Uno) binary size checks across four API styles | PlatformIO |
| **Embedded compatibility** | Library examples compiled across ESP32, AVR, STM32 via [compat-check](https://github.com/m-mcgowan/embedded-cpp-compat-check) | CI |

Coverage is tracked on both targets:

- **Host:** GCC + lcov 2.x — lines 97.5%, functions 99.0%, branches 96.2%. Enforced in CI.
- **Embedded (ESP32-S3):** lines 81.6%, functions 82.7%, gathered via [pio-cov](https://github.com/m-mcgowan/pio-cov), a PlatformIO-aware coverage runner.

Host tests run in ~35 seconds. The full CI matrix (5 compilers + coverage + embedded compat + Wokwi runtime) runs on every push.

### Documentation

The library docs are kept honest by automated checks that run pre-push and in CI:

- **Internal link verification** — every internal Markdown link in the docs tree is resolved at push time. Broken links fail the pre-push hook before they reach the remote.
- **Live code snippets** — examples in the docs are injected from compiled source files via `<!-- snippet: -->` markers, so the code in a doc is the same code that builds in CI. They can't drift from working examples.
- **Migration-table alignment** — the side-by-side note-c ↔ note-cpp tables in the migration guide are kept column-aligned by tooling, so the layout doesn't degrade as patterns are added.

All three checks run via [`tools/verify-docs.sh`](tools/verify-docs.sh), wired into both the `pre-push` git hook ([`.githooks/pre-push`](.githooks/pre-push)) and the main CI pipeline (`ci.sh`). Failures block.

## Documentation

`note-cpp` ships full prose documentation alongside the headers. The four pointers below cover the most common entry points; [`docs/README.md`](docs/README.md) is the full index.

- **Start here:** [Getting started](docs/getting-started.md) — top-down walkthrough from a clean project to your first request
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
