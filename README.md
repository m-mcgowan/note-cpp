# note-cpp

[![CI](https://github.com/m-mcgowan/note-cpp/actions/workflows/ci.yml/badge.svg)](https://github.com/m-mcgowan/note-cpp/actions/workflows/ci.yml)

Type-safe C++ API for the [Blues Notecard](https://blues.com/notecard). Requires C++20 or later (C++23 recommended for full feature set). Header-only, zero dependencies beyond the standard library.

> **Community project.** Not affiliated with or supported by Blues Inc. Notecard is a trademark of Blues Inc.

## Why note-cpp?

The Notecard C API is *stringly-typed* — field names are strings, types are manual, typos compile fine and fail at runtime. `note-cpp` provides the Notecard API in a type-safe way, while also still allowing notes to be defined by the application, via type-safe structs. Requests and responses have typed fields and IDE auto-completion — misspelled field names, wrong types, and missing required fields are all compile errors. Targeting for the specific Notecard radio technologies and firmware version is also supported. [See the full comparison with note-c.](docs/comparison.md)

The API supports fluent setters

```cpp
// examples/getting_started.cpp#L193-L197

api.hubSet()
   .product("com.example.app")
   .mode("periodic")
   .outbound(60)
   .execute();
```

as well as property assignment

```cpp
// examples/getting_started.cpp#L202-L206

auto req = api.hubSet();
req.product = "com.example.app";
req.mode = "periodic";
req.outbound = 60;
req.execute();
```

Your custom types can also be included, such as when creating notes

```cpp
// examples/getting_started.cpp#L254-L255

Readings r{.temperature = 22.5f, .humidity = 60};
api.noteAdd().file("sensors.qo").body(r).execute();
```

Inline is fine too

```cpp
// examples/getting_started.cpp#L260-L263

api.noteAdd()
   .file("sensors.qo")
   .body(Readings{.temperature = 22.5f, .humidity = 60})
   .execute();
```

and property assignment

```cpp
// examples/getting_started.cpp#L268-L274

Readings r;
r.temperature = 22.5f;
r.humidity = 60;
auto req = api.noteAdd();
req.file = "sensors.qo";
req.body(r);
req.execute();
```

## Architecture

```
┌────────────────────────────────────────────────┐
│  Your application  (optionally note-cpp-app)   │
├────────────────────────────────────────────────┤
│  Typed API layer              note-cpp         │
│    Generated requests & responses              │
│    Body structs · Note templates · targeting   │
├────────────────────────────────────────────────┤
│  Protocol layer               note-cpp         │
│    Notecard serial & I2C framing               │
│    CRC · retry · segmented TX/RX               │
├───────────────────────┬────────────────────────┤
│  JSON backend         │  Platform HAL          │
│  Default or custom    │  note-cpp-arduino      │
│                       │  note-cpp-zephyr       │
└───────────────────────┴────────────────────────┘
```

note-cpp owns everything above the bottom row. Platform libraries provide the HAL — pick one for your platform:

- **note-cpp-arduino** — Arduino (`HardwareSerial` + `Wire`)
- **note-cpp-zephyr** — Zephyr RTOS (UART + I2C)
- **note-cpp-espidf** — ESP-IDF (UART + I2C)
- **note-cpp-linux** — Linux (`/dev/ttyACM0`, `/dev/i2c-N`)

The JSON backend is pluggable — a default is provided, or you can use your own (cJSON, nlohmann-json, etc.).

## Quick Start

Once you have a `Notecard` instance, the typed API is the same everywhere:

```cpp
#include <note/api_context.hpp>
auto nc = ....;                     // we'll get to this later.
note::Api api(nc);

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

See the [getting started example](examples/getting_started.cpp) for a complete working example.

## Features

- [API Types](#generated-api-types) — typed requests and responses for all Notecard APIs
- [Target Filtering](#target-filtering) — APIs not relevant to your Notecard are deprecated or optionally rejected at compile time (C++20)
- [Polymorphic APIs](#polymorphic-apis) — handles Notecard APIs with overloaded behavior depending on the request
- [Body Values](#body-values) — raw JSON, builder lambdas, or typed structs
- [Body Structs and Note Templates](#body-structs-and-note-templates) — one struct for send, receive, and template registration
- [Error Handling](#error-handling) — `Result<T>` with structured errors and retry guidance
- [Protocol Layer](#protocol-layer) — Notecard serial and I2C protocol implementations with CRC, retry, and chunking
- [JSON Backend](#json-backend) — plug in any JSON library
- [JSON Buffer Builder](#json-buffer-builder) — compile-time or runtime JSON building, same API, no allocations

---

## Generated API Types

Request and response types covering all Notecard APIs (74 of them!) are auto-generated from the [Notecard OpenAPI spec](notecard-api.openapi.json). Each has typed fields, chainable setters, and an `execute()` method. Fields support fluent chaining, direct assignment, and designated initializers.

```cpp
api.hubSet()
    .product("com.example.app")
    .mode("periodic")
    .outbound(60)
    .execute();
```

Requests and responses also support

- `extras` for undocumented properties (`extra(key, value)`)
- string key access (`req["mode"] = ...`)
- fire-and-forget commands (`.command()`)
- compile-time enum validation (`validatedMode("periodic")`)

Include everything with `#include <note/api_context.hpp>`.

---

## Target Filtering

When targeting a specific Notecard product, the `Api` constructor accepts a target that provides compile-time feedback on API compatibility (C++20). Unsupported APIs produce deprecation warnings, or compile errors in strict mode. Built-in targets include `Product::WiFi`, `Product::Cell`, `Product::LoRa`, and `Product::Skylo`, and you can compose custom targets with additional RATs (e.g. `Product::Cell + Rat::Ntn`).

```cpp
note::Api wifi(nc, note::target<note::Product::WiFi>());
wifi.cardSleep();  // OK: card.sleep supports WiFi
wifi.hubSet();     // OK: universal
```

Each API type carries `static constexpr Skus skus` for introspection. See [examples/target_filtering.cpp](examples/target_filtering.cpp).

---

## Polymorphic APIs

Some Notecard APIs have overloaded behavior — the behavior and response depends on which fields you send. `note-cpp` models each behavior as a named sub-operation with two equivalent access styles:

```cpp
api.noteGet().get().file("data.qi").execute();   // start from the API name
api.getNoteGet().file("data.qi").execute();       // start from the action
```

The same pattern applies to `card.binary`, `card.contact`, `card.location.mode`, `card.power`, `card.temp`, `note.template`, and others.

---

## Body Values

Note bodies support three tiers: **raw JSON strings**, **builder lambdas**, and **typed structs**. The struct approach is the most powerful — a single struct serves as builder, parser, and definition for Note template registration. See [Body Structs and Note Templates](#body-structs-and-note-templates).

```cpp
// examples/getting_started.cpp#L254-L255

Readings r{.temperature = 22.5f, .humidity = 60};
api.noteAdd().file("sensors.qo").body(r).execute();
```

---

## Body Structs and Note Templates

Define a struct once, use it to send data, receive data, and register [Notecard templates](https://dev.blues.io/notecard/notecard-walkthrough/low-bandwidth-design/#notecard-templates). On C++20+, plain aggregates work automatically. On C++17, add `NOTE_FIELDS(...)`.

```cpp
// examples/getting_started.cpp#L75-L79

struct Readings {
    float temperature;
    int16_t humidity;
    NOTE_FIELDS(temperature, humidity)
};
```

```cpp
// examples/getting_started.cpp#L280-L282

api.noteTemplate().set("sensors.qo")
    .body(note::template_of<Readings>())
    .execute();
```

```cpp
// examples/getting_started.cpp#L287-L290

auto r = api.noteGet().get().file("data.qi").execute();
if (r) {
    Readings data = r.bodyAs<Readings>();
    (void)data.temperature;
```

See [examples/sending-notes/](examples/sending-notes/) for a complete walkthrough.

---

## Error Handling

All operations return a result that is truthy on success. On failure, `error()` provides a structured `ErrorInfo` with an error code and message.

```cpp
// examples/getting_started.cpp#L212-L222

auto result = api.cardVersion().execute();
if (result) {
    auto version = result.version;
    auto device  = result.device;
    (void)version; (void)device;
} else {
    auto err = result.error();
    // err.code    — Error enum (Transport, Protocol, Notecard, ...)
    // err.message — human-readable description
    (void)err;
}
```

Each request also carries a compile-time safety level (`ReadOnly`, `Idempotent`, `NonIdempotent`, `Destructive`) that your transport or retry logic can inspect to decide whether it's safe to retry a failed request.

---

## Protocol Layer

Header-only implementations of the Notecard serial and I2C protocols: `NotecardSerial` and `NotecardI2c`. These implement the same wire protocols as note-c's `n_serial.c` and `n_i2c.c` — CRC auto-detection, segmented TX/RX, retry logic, and auto-reset — so your application talks to the Notecard using the standard framing it expects.

Each protocol takes a thin platform HAL - a lightweight virtual interface for UART or I2C hardware access. Platform libraries like `note-cpp-arduino` and `note-cpp-zephyr` provide ready-made HAL implementations.

See [docs/transport.md](docs/transport.md) for the full HAL interface and implementation notes.

---

## JSON Backend

The Notecard speaks JSON — every request is serialized to a JSON string, and every response is parsed from one. `note-cpp` handles this behind the scenes through a `JsonBackend` interface, so your application code works with typed fields, not raw JSON. You just need to tell `note-cpp` which JSON library to use.

Ready-made backends are provided for:

- **cJSON** — `#include <note/backends/cjson.hpp>` (bundled with note-c and ESP-IDF)
- **nlohmann-json** — `#include <note/backends/nlohmann.hpp>`

Each is a single header, auto-detected via `__has_include`. To use a different JSON library, implement the `JsonBackend` interface — see [examples/getting_started.cpp](examples/getting_started.cpp).

---

## JSON Buffer Builder

`note::json` builds JSON into a fixed-size buffer with no allocations. When all values are constants, the JSON is computed entirely at compile time. Replace `constexpr` with a runtime variable and the same code works at runtime — no API change needed.

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
- [note-cpp-app design](docs/note-cpp-app.md)
