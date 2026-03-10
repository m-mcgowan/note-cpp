# note-cpp

[![CI](https://github.com/m-mcgowan/note-cpp/actions/workflows/ci.yml/badge.svg)](https://github.com/m-mcgowan/note-cpp/actions/workflows/ci.yml)

Type-safe C++ API for the [Blues Notecard](https://blues.com/notecard). Requires C++20 or later (C++23 recommended for full feature set). Header-only, zero dependencies beyond the standard library.

> **Community project.** Not affiliated with or supported by Blues Inc. Notecard is a trademark of Blues Inc.

## Why note-cpp?

The Notecard C API is stringly-typed — field names are strings, types are manual, typos compile fine and fail at runtime. note-cpp gives every request typed fields, IDE auto-completion, and compile-time safety. [See the full comparison with note-c.](docs/comparison.md)

```cpp
// Fluent chaining
api.hubSet()
   .product("com.example.app")
   .mode("periodic")
   .outbound(60)
   .execute();

// Direct assignment
auto req = api.hubSet();
req.product = "com.example.app";
req.mode = "periodic";
req.outbound = 60;
req.execute();

// Typed body structs
Readings r{.temperature = 22.5f, .humidity = 60};
api.noteAdd().file("sensors.qo").body(r).execute();
```

## Quick Start

Platform libraries provide the backend and transport — pick the one for your platform:

- **note-cpp-arduino** — Arduino (`HardwareSerial` + `Wire`)
- **note-cpp-zephyr** — Zephyr RTOS (UART + I2C)
- **note-cpp-espidf** — ESP-IDF (UART + I2C)
- **note-cpp-linux** — Linux (`/dev/ttyACM0`, `/dev/i2c-N`)

Once you have a `Notecard` instance, the typed API is the same everywhere:

```cpp
#include <note/api_context.hpp>

auto api = note::make_api(nc);

// Make requests. Fields are typed, IDE auto-completes everything.
api.hubSet()
   .product("com.example.app")
   .mode("periodic")
   .outbound(60)
   .execute();

// Read responses. Fields are named members, not strings.
auto result = api.cardVersion().execute();
if (result) {
    auto version = result.version;   // string_view
    auto device  = result.device;    // string_view
} else {
    auto err = result.error();       // ErrorInfo{code, message}
}
```

You can also wire up a custom backend and transport directly — see [Backend Interfaces](#backend-interfaces) and the [getting started example](examples/getting_started.cpp).

## Features

- [Generated API Types](#generated-api-types) — typed requests for every Notecard API
- [Target Filtering](#target-filtering) — APIs not relevant to your Notecard are deprecated or rejected at compile time (C++20)
- [Polymorphic APIs](#polymorphic-apis) — Notecard APIs with overloaded behavior depending on the request
- [Body Values](#body-values) — raw JSON, builder lambdas, or typed structs
- [Body Structs and Note Templates](#body-structs-and-note-templates) — one struct for send, receive, and template registration
- [Error Handling](#error-handling) — `Result<T>` with structured errors and retry guidance
- [Backend Interfaces](#backend-interfaces) — plug in any JSON library and transport
- [Transport Layer](#transport-layer) — serial and I2C with CRC, retry, and chunking
- [JSON Buffer Builder](#json-buffer-builder) — compile-time or runtime JSON, same API, no allocations

---

## Generated API Types

Request/response types covering all 74 Notecard APIs are auto-generated from the [Notecard OpenAPI spec](notecard-api.openapi.json). Each has typed fields, chainable setters, and an `execute()` method. Fields support fluent chaining, direct assignment, and designated initializers.

```cpp
api.hubSet().product("com.example.app").mode("periodic").outbound(60).execute();
```

Extras for undocumented properties (`extra(key, value)`), string key access (`req["mode"] = ...`), fire-and-forget commands (`.command()`), and compile-time enum validation (`validatedMode("periodic")`) are all built in. Include everything with `#include <note/api_context.hpp>`.

---

## Target Filtering

When targeting a specific Notecard product, `make_api()` provides compile-time feedback on API compatibility (C++20). Unsupported APIs produce deprecation warnings, or compile errors in strict mode.

```cpp
auto wifi = note::make_api(nc, note::target<note::Product::WiFi>());
wifi.cardSleep();  // OK: card.sleep supports WiFi
wifi.hubSet();     // OK: universal endpoint
```

Each API type carries `static constexpr Skus skus` for introspection. See [examples/target_filtering.cpp](examples/target_filtering.cpp).

---

## Polymorphic APIs

Some Notecard APIs have overloaded behavior — the response depends on which fields you send. note-cpp models each behavior as a named sub-operation with two equivalent access styles:

```cpp
api.noteGet().get().file("data.qi").execute();   // endpoint-first
api.getNoteGet().file("data.qi").execute();       // action-first
```

The same pattern applies to `card.binary`, `card.contact`, `card.location.mode`, `card.power`, `card.temp`, `note.template`, and others.

---

## Body Values

Note bodies support three tiers: **raw JSON strings**, **builder lambdas**, and **typed structs**. The struct approach is the most powerful — a single struct serves as builder, parser, and Note template registration. See [Body Structs and Note Templates](#body-structs-and-note-templates).

```cpp
api.noteAdd().file("sensors.qo").body(readings).execute();
```

---

## Body Structs and Note Templates

Define a struct once, use it to send data, receive data, and register [Notecard templates](https://dev.blues.io/notecard/notecard-walkthrough/low-bandwidth-design/#notecard-templates). On C++20+, plain aggregates work automatically. On C++17, add `NOTE_FIELDS(...)`.

```cpp
struct Readings {
    float temperature;
    int16_t humidity;
    NOTE_FIELDS(temperature, humidity)  // not needed on C++20+
};

// Send
api.noteAdd().file("sensors.qo").body(Readings{22.5f, 60}).execute();

// Register template (auto-generates Notecard type hints)
api.noteTemplate().set("sensors.qo").body(note::template_of<Readings>()).execute();

// Receive
auto r = api.noteGet().get().file("data.qi").execute();
if (r) auto data = r.bodyAs<Readings>();
```

See [examples/sending-notes/](examples/sending-notes/) for a complete walkthrough.

---

## Error Handling

All operations return a result that is truthy on success. Each request carries a compile-time safety level (`ReadOnly`, `Idempotent`, `NonIdempotent`, `Destructive`) for retry decisions.

```cpp
auto result = api.cardVersion().execute();
if (result) {
    auto version = result.version;
} else {
    auto err = result.error();  // ErrorInfo{code, message}
}
```

API version gating restricts available fields to a specific firmware version — define `NOTE_API_VERSION` before including endpoint headers.

---

## Backend Interfaces

note-cpp is transport-agnostic. You provide a `JsonBackend` (wraps your JSON library) and a transport callable (sends/receives JSON strings). See [examples/getting_started.cpp](examples/getting_started.cpp) for a complete working example.

```cpp
note::Notecard nc(backend,
    [](note::string_view request, uint32_t timeout_ms) -> note::Result<std::string> {
        return my_transport(request, timeout_ms);
    });
```

---

## Transport Layer

Header-only implementations of both Notecard wire protocols: `NotecardSerial` and `NotecardI2c`. Both handle CRC auto-detection, segmented TX, retry logic, and auto-reset. Each takes a platform HAL — a small virtual interface for your target hardware.

See [docs/transport.md](docs/transport.md) for the full HAL interface and implementation notes.

---

## JSON Buffer Builder

`JsonBuf<N>` builds JSON into a fixed-size buffer with no allocations. When all values are constants, the JSON is computed entirely at compile time. Replace `constexpr` with a runtime variable and the same code works at runtime — no API change needed.

```cpp
constexpr auto req = note::json<[](auto& b) {
    b.add("req", "hub.set");
    b.add("mode", "periodic");
    b.close();
}>();
static_assert(req.view() == R"({"req":"hub.set","mode":"periodic"})");
```

---

## Code Generation

The API types are generated from the Notecard OpenAPI spec:

```bash
pip install jinja2
python3 tools/codegen/generate.py notecard-api.openapi.json
```

## Building and Testing

```bash
./ci.sh                  # default compiler
./ci.sh --all-compilers  # all locally installed compilers
./ci.sh --coverage       # coverage report (requires GCC 13+ and lcov 2.x)
```

Runs code generation, header compilation checks, unit tests, and examples. See [docs/coverage.md](docs/coverage.md) for coverage details (~98.5% lines, ~99.9% functions).

## Documentation

- [Why note-cpp? (comparison with note-c)](docs/comparison.md)
- [Transport layer](docs/transport.md)
- [Coverage](docs/coverage.md)
- [Project plan and status](docs/PLAN.md)
- [note-app design](docs/note-app.md)
