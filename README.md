# note-cpp

[![CI](https://github.com/m-mcgowan/note-cpp/actions/workflows/ci.yml/badge.svg)](https://github.com/m-mcgowan/note-cpp/actions/workflows/ci.yml)

Type-safe C++ API for the [Blues Notecard](https://blues.com/notecard). Requires C++20 or later (C++23 recommended for full feature set). Header-only, zero dependencies beyond the standard library.

> **Community project.** Not affiliated with or supported by Blues Inc. Notecard is a trademark of Blues Inc.

## Why note-cpp?

The Notecard C API is stringly-typed — field names are strings, types are manual, typos compile fine and fail at runtime. note-cpp gives every request typed fields, IDE auto-completion, and compile-time safety. [See the full comparison with note-c.](docs/comparison.md)

```cpp
api.hubSet()
   .product("com.example.app")
   .mode("periodic")
   .outbound(60)
   .execute();
```

## Quick Start

note-cpp has two integration points: a **JSON backend** (wraps your JSON library) and a **transport** (sends/receives strings over serial or I2C). Both are simple to implement — see the [full getting started example](examples/getting_started.cpp) which compiles and runs with a mock backend.

```cpp
#include <note/api_context.hpp>

// 1. Create a Notecard with your backend and transport.
MyJsonBackend backend;
note::Notecard nc(backend,
    [](note::string_view request, uint32_t timeout_ms) -> note::Result<std::string> {
        return my_serial_send(request, timeout_ms);  // your transport
    });

// 2. Create an Api instance — the entry point for all typed requests.
auto api = note::make_api(nc);

// 3. Make requests. Fields are typed, IDE auto-completes everything.
api.hubSet()
   .product("com.example.app")
   .mode("periodic")
   .outbound(60)
   .execute();

// 4. Read responses. Fields are named members, not strings.
auto result = api.cardVersion().execute();
if (result) {
    auto version = result.version;   // string_view
    auto device  = result.device;    // string_view
} else {
    auto err = result.error();       // ErrorInfo{code, message}
}
```

```bash
c++ -std=c++2b -I include examples/getting_started.cpp && ./a.out
```

## Features

- [Generated API Types](#generated-api-types) — 74 typed endpoints from the OpenAPI spec
- [Target Filtering](#target-filtering) — compile-time SKU/product compatibility (C++20)
- [Polymorphic Endpoints](#polymorphic-endpoints) — named sub-operations for multi-mode endpoints
- [Body Values](#body-values) — raw JSON, builder lambdas, or typed schema structs
- [Schemas and Templates](#schemas-and-templates) — one struct for send, receive, and template registration
- [Error Handling](#error-handling) — `Result<T>` with safety classification and version gating
- [Backend Interfaces](#backend-interfaces) — plug in any JSON library and transport
- [Transport Layer](#transport-layer) — serial and I2C with CRC, retry, and chunking
- [JSON Buffer Builder](#json-buffer-builder) — `constexpr` stack-allocated JSON construction

---

## Generated API Types

74 request/response types are auto-generated from the [Notecard OpenAPI spec](notecard-api.openapi.json). Each has typed fields, chainable setters, and an `execute()` method. Fields support fluent chaining, direct assignment, and designated initializers.

```cpp
api.hubSet().product("com.example.app").mode("periodic").outbound(60).execute();
```

Extras for undocumented properties (`extra(key, value)`), string key access (`req["mode"] = ...`), fire-and-forget commands (`.command()`), and compile-time enum validation (`validatedMode("periodic")`) are all built in. Include everything with `#include <note/api_context.hpp>`.

---

## Target Filtering

When targeting a specific Notecard product, `make_api()` provides compile-time feedback on endpoint compatibility (C++20). Unsupported endpoints produce deprecation warnings, or compile errors in strict mode.

```cpp
auto wifi = note::make_api(nc, note::target<note::Product::WiFi>());
wifi.cardSleep();  // OK: card.sleep supports WiFi
wifi.hubSet();     // OK: universal endpoint
```

Each endpoint carries `static constexpr Skus skus` for introspection. See [examples/target_filtering.cpp](examples/target_filtering.cpp).

---

## Polymorphic Endpoints

Some endpoints behave differently depending on which fields you send. note-cpp models these as named sub-operations with two equivalent access styles:

```cpp
api.noteGet().get().file("data.qi").execute();   // endpoint-first
api.getNoteGet().file("data.qi").execute();       // action-first
```

The same pattern applies to `card.binary`, `card.contact`, `card.location.mode`, `card.power`, `card.temp`, `note.template`, and others.

---

## Body Values

Note bodies support three tiers: **raw JSON strings**, **builder lambdas**, and **typed schema structs**. The schema struct approach is the most powerful — a single struct serves as builder, parser, and template registration. See [Schemas and Templates](#schemas-and-templates).

```cpp
api.noteAdd().file("sensors.qo").body(readings).execute();
```

---

## Schemas and Templates

Define a struct once, use it to send data, receive data, and register Notecard templates. On C++20+, plain aggregates work automatically. On C++17, add `NOTE_FIELDS(...)`.

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

`JsonBuf<N>` writes JSON into a fixed-size buffer with no allocations. Fully `constexpr` — when all values are compile-time constants, the JSON string is computed at build time. Auto-sizing via `note::json<>()` lets the compiler pick the buffer size.

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

The 74 endpoint types are generated from the OpenAPI spec:

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
