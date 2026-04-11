# note-cpp

[![CI](https://github.com/m-mcgowan/note-cpp/actions/workflows/ci.yml/badge.svg)](https://github.com/m-mcgowan/note-cpp/actions/workflows/ci.yml)

Type-safe C++ API for the [Blues Notecard](https://blues.com/notecard). Header-only, zero dependencies beyond the standard library. Works with C++17, C++20, and C++23 — each version unlocks additional features.

> **Community project.** Not affiliated with or supported by Blues Inc. Notecard is a trademark of Blues Inc.

## Quick Start

### Arduino

```cpp
#include <note.hpp>

Notecard nc;

void setup() {
    nc.begin(Serial1, 9600);       // serial — or nc.begin(Wire) for I2C

    nc.hub.set()
        .product("com.example.app")
        .mode("periodic")
        .execute();
}
```

Install via Arduino Library Manager or `arduino-cli lib install note-cpp`.

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

```cpp
nc.hub.set()
   .product("com.example.app")
   .mode("periodic")
   .outbound(60_mins)
   .execute();
```

Or use direct assignment — natural when fields come from config:

```cpp
auto req = nc.hub.set();
req.product = "com.example.app";
req.mode = "periodic";
req.outbound = 60;
req.execute();
```

Send typed data with body structs — define once, use for send, receive, and template registration:

```cpp
struct Readings {
    float temperature;
    int16_t humidity;
    NOTE_FIELDS(temperature, humidity)  // optional on C++20
};

nc.note.add()
   .file("sensors.qo")
   .body(Readings{.temperature = 22.5f, .humidity = 60})
   .execute();
```

Read responses with typed fields:

```cpp
auto rsp = nc.card.version().execute();
if (rsp) {
    auto version = rsp.version;   // string_view
    auto device  = rsp.device;    // string_view
} else {
    auto err = to_string(rsp.error());
}
```

See the [getting started example](examples/getting_started.cpp) for a complete walkthrough.

> **Coming from note-arduino / note-c?** The
> [migration guide](docs/guides/migration-from-note-arduino.md) has side-by-side
> examples covering setup, hub.set, note.add, templates, error handling,
> binary transfers, and more.

## Features

<details>
<summary><strong>Generated API Types</strong> — typed requests and responses for all 74 Notecard APIs</summary>

Request and response types are auto-generated from the [Notecard OpenAPI spec](notecard-api.openapi.json). Each has typed fields, chainable setters, and an `execute()` method.

```cpp
nc.hub.set()
    .product("com.example.app")
    .mode("periodic")
    .outbound(60)
    .execute();
```

Requests and responses also support `extras` for undocumented properties, string key access (`req["mode"] = ...`), fire-and-forget commands (`.command()`), and compile-time enum validation (`validatedMode("periodic")`). Include everything with `#include <note/api.hpp>`.

See [API name mapping](docs/api-patterns.md) for how Notecard request names map to C++ methods.

</details>

<details>
<summary><strong>Intent-Scoped APIs</strong> — distinct types for multi-purpose endpoints</summary>

Some Notecard APIs behave differently depending on which fields you send. In `note-cpp`, each intent is a distinct type with only the fields that apply:

```cpp
// Read a Note by ID
auto rsp = nc.note.read("data.db").noteId("my-note").execute();

// Pop from queue
auto rsp = nc.note.pop("requests.qi").execute();

// card.location.mode — fixed() accepts lat/lon, get() doesn't
nc.card.locationMode().fixed()
    .lat(42.565).lon(-70.783)
    .execute();
```

Setting a field that doesn't apply to that operation is a compile error. See [docs/intent-scoped-apis.md](docs/intent-scoped-apis.md) for the full list.

</details>

<details>
<summary><strong>Body Values and Note Templates</strong> — one struct for send, receive, and template registration</summary>

Define a body struct once and use it everywhere. On C++20+, plain aggregates work automatically. On C++17, add `NOTE_FIELDS(...)`.

```cpp
// Send
nc.note.add().file("sensors.qo").body(Readings{.temperature = 22.5f, .humidity = 60}).execute();

// Receive
Readings data = rsp.bodyAs<Readings>();

// Register template (auto-generates type hints for compact storage)
nc.note.templates().define("sensors.qo").body(note::template_of(Readings())).execute();
```

Bodies can also be set with `json_fmt` (C++20, compile-time validated), builder lambdas, or raw strings. See [docs/body-values.md](docs/body-values.md).

</details>

<details>
<summary><strong>Type-Safe Duration Units</strong> — <code>Minutes</code>, <code>Seconds</code>, <code>Hours</code>, <code>Days</code> with compile-time safety</summary>

Duration fields use distinct types that prevent accidental unit mixing. Larger units implicitly convert to smaller ones — `7_days` where `Minutes` is expected does the math at compile time.

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

Also includes voltage-variable sync builders and comma-separated flag fields with named methods. See [docs/duration-units.md](docs/duration-units.md) and [examples/hub-configuration/](examples/hub-configuration/).

</details>

<details>
<summary><strong>Error Handling</strong> — <code>Result&lt;T&gt;</code> with structured errors and retry guidance</summary>

All operations return a result that is truthy on success. On failure, `error()` provides a structured `ErrorInfo` with an error code, cause, and message.

```cpp
auto result = nc.card.version().execute();
if (result) {
    auto version = result.version;
} else {
    ErrorInfo err = result.error();
    err.code;     // Error::ResponseLost, Error::Notecard, etc.
    err.cause;    // Cause::Timeout, Cause::HalError, etc.
    err.message;  // "no response within deadline"
    printf("error: %s\n", to_string(err).c_str());
}
```

Each request carries a compile-time safety level (`ReadOnly`, `Idempotent`, `NonIdempotent`, `Destructive`) for retry decisions. See [docs/error-handling.md](docs/error-handling.md).

</details>

<details>
<summary><strong>Target Filtering</strong> — compile-time feedback on API compatibility by Notecard SKU (C++20)</summary>

The `Api` constructor accepts a target that warns (or errors in strict mode) when an API isn't available on your Notecard product.

```cpp
note::Api<note::Product::WiFi> nc(notecard);
nc.card.sleep();  // OK: card.sleep supports WiFi
nc.hub.set();     // OK: universal
```

Built-in targets include `Product::WiFi`, `Product::Cell`, `Product::LoRa`, and `Product::Skylo`. Custom targets can compose additional RATs (e.g. `Product::Cell + Rat::Ntn`). See [examples/target_filtering.cpp](examples/target_filtering.cpp).

</details>

<details>
<summary><strong>Streaming Architecture</strong> — zero-heap operation on constrained devices</summary>

Requests are serialized directly to the transport as fields are set — no request buffer is ever held in memory. Responses are parsed with a SAX (event-driven) parser that populates struct fields as bytes arrive from the wire.

On an Arduino Uno (ATmega328P, 32 KB flash / 2 KB RAM), an 8-endpoint application:

| | note-c | `note-cpp` | Delta |
|---|---|---|---|
| **Flash** | 25,076 (78%) | 26,488 (82%) | +1,412 |
| **Static RAM** | 729 (36%) | 832 (41%) | +103 |
| **Heap (peak)** | 371 (18%) | 0 (0%) | **-371** |
| **Total RAM** | 1,100 (54%) | 832 (41%) | **-24%** |

All memory is statically allocated at compile time using `MonotonicArena` and `StaticNotecard`. See [docs/feature-flags.md](docs/feature-flags.md) for the compile-time options that enable this.

</details>

<details>
<summary><strong>Wire Protocols</strong> — serial and I2C with CRC, retry, and binary transfer</summary>

Header-only implementations of the Notecard serial and I2C protocols: `NotecardSerial` and `NotecardI2c`. These handle CRC auto-detection, segmented TX/RX, retry logic, and auto-reset.

Each protocol takes a thin platform HAL — a lightweight virtual interface for UART or I2C hardware access. See [docs/transport.md](docs/transport.md) for the full HAL interface.

Binary transfer APIs (`card.binary.get`, `card.binary.put`) use COBS framing handled internally by the transport. See [docs/binary-transfer.md](docs/binary-transfer.md).

An optional JSONB binary wire format (`NOTE_JSONB`) replaces JSON text with compact binary opcodes for reduced overhead on numeric-heavy payloads. See [docs/jsonb.md](docs/jsonb.md).

</details>

<details>
<summary><strong>JSON Backend and Builder</strong> — pluggable backend, zero-allocation constexpr builder</summary>

The Notecard's JSON wire format is an implementation detail — your application works with typed structs. A default backend works out of the box. See [docs/json-backend.md](docs/json-backend.md) for customization options.

`note::json` builds JSON into a fixed-size buffer with no allocations. When all values are constants, the JSON is computed entirely at compile time:

```cpp
constexpr auto req = note::json<[](auto& b) {
    b.add("req", "hub.set");
    b.add("mode", "periodic");
    b.close();
}>();
static_assert(req.view() == R"({"req":"hub.set","mode":"periodic"})");
```

See [docs/json-builder.md](docs/json-builder.md).

</details>

### C++ Version Compatibility

The core library works with C++17. Each successive standard unlocks additional features:

| Feature | C++17 | C++20 | C++23 |
|---------|:-----:|:-----:|:-----:|
| **Core** | | | |
| Typed API (request builders, responses, fluent setters) | yes | yes | yes |
| Ad-hoc requests (`nc.request("hub.set", lambda)`) | yes | yes | yes |
| Error handling (`Result<T>`, structured errors) | yes | yes | yes |
| Type-safe duration units (`Seconds`, `Minutes`, `Hours`, `Days`) | yes | yes | yes |
| **JSON** | | | |
| JSON backends (cJSON, nlohmann, buffer/jsmn) | yes | yes | yes |
| SAX streaming parser (`JsonSink`) | yes | yes | yes |
| `JsonBuf` runtime builder (no allocations) | yes | yes | yes |
| `consteval` JSON (`note::json<>()`) | — | yes | yes |
| **Body structs** | | | |
| Body structs with `NOTE_FIELDS` macro | yes | yes | yes |
| Body structs without macro (plain aggregates via reflection) | — | yes | yes |
| **Compile-time checks** | | | |
| `consteval` enum validation (`validatedMode()`) | — | yes | yes |
| Target filtering (compile-time SKU/RAT checks) | — | yes | yes |
| Version gating (firmware-version field availability) | yes | yes | yes |
| **Transport** | | | |
| `ITransport` interface (virtual) | yes | yes | yes |
| `AbstractTransport` (shared retry/CRC) | yes | yes | yes |
| Serial and I2C protocol implementations | yes | yes | yes |
| `CallbackTransport` (lambda adapter for testing) | yes | yes | yes |
| **Memory** | | | |
| `MonotonicArena` + arena allocator | yes | yes | yes |
| `StringPool` response string interning | yes | yes | yes |
| Zero-alloc `BufferJsonBackend` (jsmn) | yes | yes | yes |
| **Standard library** | | | |
| `std::expected` (native, vs `tl::expected` fallback) | — | — | yes |
| `std::unreachable` (native, vs compiler builtins) | — | — | yes |

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
| **Code coverage** | GCC 13 + lcov 2.3 — lines 96%, functions 98%, branches 89% | CI enforced |
| **Multi-compiler CI** | g++ 12/13/14, clang++ 17/18, C++20 and C++23, libstdc++ and libc++ | 5 configurations |
| **On-device integration** | ESP32-S3 with a real Notecard over serial — API requests, body parsing, binary transfer, streaming SAX | 36 test cases |
| **AVR build verification** | ATmega328P (Arduino Uno) binary size comparison against note-c | PlatformIO |
| **Embedded compatibility** | Library examples compiled across ESP32, AVR, STM32 via [compat-check](https://github.com/m-mcgowan/embedded-cpp-compat-check) | CI |

Host tests run in ~35 seconds. The full CI matrix (5 compilers + coverage + embedded compat) runs on every push.

## Documentation

- [Migrating from note-arduino](docs/guides/migration-from-note-arduino.md) — side-by-side examples for common patterns
- [Feature flags](docs/feature-flags.md) — compile-time options for binary size optimization (AVR, Cortex-M0)
- [Full documentation index](docs/README.md) — all guides, from getting started to internals
- [API reference (Doxygen)](https://m-mcgowan.github.io/note-cpp/)

## Contributing

Requires C++17 minimum. C++20 enables zero-overhead transport policies
and compile-time validation of enum string fields.

Bug reports, feature requests, and pull requests are welcome via
[GitHub Issues](https://github.com/m-mcgowan/note-cpp/issues) and
[Pull Requests](https://github.com/m-mcgowan/note-cpp/pulls).

See [CONTRIBUTING.md](CONTRIBUTING.md) for development setup, building,
testing, and code generation details.
