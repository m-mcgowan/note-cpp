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
// examples/getting_started.cpp#L250-L253

{
    Readings r{.temperature = 22.5f, .humidity = 60};
    api.noteAdd().file("sensors.qo").body(r).execute();
}
```

Inline is fine too

```cpp
// examples/getting_started.cpp#L257-L260

api.noteAdd()
   .file("sensors.qo")
   .body(Readings{.temperature = 22.5f, .humidity = 60})
   .execute();
```

and property assignment

```cpp
// examples/getting_started.cpp#L264-L272

{
    Readings r;
    r.temperature = 22.5f;
    r.humidity = 60;
    auto req = api.noteAdd();
    req.file = "sensors.qo";
    req.body(r);
    req.execute();
}
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
│  (customizable)       │  note-cpp-arduino      │
│                       │  note-cpp-zephyr       │
└───────────────────────┴────────────────────────┘
```

note-cpp owns everything above the bottom row. Platform libraries provide the HAL — pick one for your platform:

- **note-cpp-arduino** — Arduino (`HardwareSerial` + `Wire`)
- **note-cpp-zephyr** — Zephyr RTOS (UART + I2C)
- **note-cpp-espidf** — ESP-IDF (UART + I2C)
- **note-cpp-linux** — Linux (`/dev/ttyACM0`, `/dev/i2c-N`)

The JSON backend works out of the box. It's customizable if you have specific resource or tooling constraints — see [docs/json-backend.md](docs/json-backend.md).

## Quick Start

Once you have a `Notecard` instance, the typed API is the same everywhere:

```cpp
#include <note/api_context.hpp>
using namespace note::literals;
auto nc = ....;                     // we'll get to this later.
note::Api api(nc);

// Make requests. Fields are typed, IDE auto-completes everything.
api.hubSet()
   .product("com.example.app")
   .mode("periodic")
   .outbound(60_mins)
   .execute();

// Read responses. Fields are named members, not strings.
auto result = api.cardVersion().execute();
if (result) {
    auto version = result.version;   // string_view
    auto device  = result.device;    // string_view
} else {
    auto err = to_string(result.error());  // "transport: I2C NACK"
}
```

See the [getting started example](examples/getting_started.cpp) for a complete working example.

## Features

- [API Types](#generated-api-types) — typed requests and responses for all Notecard APIs
- [Target Filtering](#target-filtering) — APIs not relevant to your Notecard are deprecated or optionally rejected at compile time (C++20)
- [Polymorphic APIs](#polymorphic-apis) — handles Notecard APIs with overloaded behavior depending on the request
- [Body Values](#body-values) — raw JSON, builder lambdas, or typed structs
- [Body Structs and Note Templates](#body-structs-and-note-templates) — one struct for send, receive, and template registration
- [Type-Safe Duration Units](#type-safe-duration-units) — `Minutes`, `Seconds`, `Hours`, `Days` with implicit conversion and compile-time safety
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

Some Notecard APIs behave differently depending on which fields you send. In `note-c` these share a single function — you pass the right combination of fields and hope you didn't set one that's irrelevant or wrong. In `note-cpp`, each behavior is a **distinct type** with only the fields that apply, its own response type, and a compile-time safety level:

```cpp
// Read a Note (ReadOnly — safe to retry on failure)
auto r = api.noteGet().get().file("data.qi").execute();

// Pop from queue (Destructive — not safe to retry blindly)
auto r = api.noteGet().delete_().file("requests.qi").execute();

// card.location.mode — Set accepts lat/lon, Get and Delete don't
api.cardLocationMode().set()
    .mode("fixed").lat(42.565).lon(-70.783)   // lat/lon only exist on Set
    .execute();

api.cardLocationMode().get().execute();       // no lat/lon fields to misuse
```

Each variant exposes only the fields the Notecard expects for that operation — setting a field that doesn't apply is a compile error, not a silent wire-level mistake.

The same pattern applies to `card.binary`, `card.contact`, `card.location.mode`, `card.temp`, `note.template`, and others. See [docs/polymorphic-apis.md](docs/polymorphic-apis.md) for the full list.

---

## Body Values

Note bodies support three tiers: **raw JSON strings**, **builder lambdas**, and **typed structs**. The struct approach is the most powerful — a single struct serves as builder, parser, and definition for Note template registration. See [Body Structs and Note Templates](#body-structs-and-note-templates).

```cpp
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

Send typed data:

```cpp
// examples/getting_started.cpp#L250-L253

{
    Readings r{.temperature = 22.5f, .humidity = 60};
    api.noteAdd().file("sensors.qo").body(r).execute();
}
```

Register a template (auto-generates type hints `14.1` = TFLOAT32, `11` = TINT16):

```cpp
// examples/getting_started.cpp#L277-L279

api.noteTemplate().set("sensors.qo")
    .body(note::template_of<Readings>())
    .execute();
```

Parse response body back into the struct:

```cpp
// examples/getting_started.cpp#L283-L291

{
    auto r = api.noteGet().get().file("data.qi").execute();
    if (r) {
        Readings data = r.bodyAs<Readings>();
        (void)data.temperature;
        (void)data.humidity;
        std::puts("  (body parsed into Readings struct)");
    }
}
```

See [examples/sending-notes/](examples/sending-notes/) for a complete walkthrough.

---

## Type-Safe Duration Units

Duration fields across the Notecard API use distinct types (`Minutes`, `Seconds`, `Hours`, `Days`) that prevent accidental unit mixing at compile time. Larger units implicitly convert to smaller ones — write `7_days` where a `Minutes` field is expected and the library does the math.

```cpp
using namespace note::literals;

// hub.set outbound/inbound are Minutes on the wire
api.hubSet()
    .outbound(15_mins)           // Minutes literal
    .inbound(7_days)             // Days → Minutes (10080 on the wire)
    .execute();

// card.attn seconds field accepts Minutes/Hours too
api.cardAttn()
    .seconds(5_mins)             // Minutes → Seconds (300 on the wire)
    .execute();

// card.sleep — long sleep expressed naturally
api.cardSleep()
    .seconds(12_hours)           // Hours → Seconds (43200 on the wire)
    .execute();

// Compile-time safety — wrong direction is a type error:
// api.hubSet().outbound(300_s);    // error: Seconds ≠ Minutes
```

**Voltage-variable sync** — adapt sync frequency to the Notecard's supply voltage. A builder constructs the semicolon-delimited string safely:

```cpp
auto req = api.hubSet();
req.mode = "periodic";
req.voutbound.usb(5).high(15).normal(60).low(240).dead(0);
req.vinbound.usb(5).high(30).normal(120).low(1440).dead(0);
req.execute();
// produces: "voutbound":"usb:5;high:15;normal:60;low:240;dead:0"
```

Only levels you set are emitted — `.voutbound.usb(5).normal(60)` produces `"usb:5;normal:60"`.

See [examples/hub-configuration/](examples/hub-configuration/) for more.

---

## Error Handling

All operations return a result that is truthy on success. On failure, `error()` provides a structured `ErrorInfo` with an error code and message. `to_string()` formats it for logging.

```cpp
auto result = api.cardVersion().execute();
if (result) {
    auto version = result.version;   // string_view
    auto device  = result.device;    // string_view
} else {
    // Structured error with code, cause, and message:
    ErrorInfo err = result.error();
    err.code;     // Error::ResponseLost, Error::Notecard, etc.
    err.cause;    // Cause::Timeout, Cause::HalError, etc.
    err.message;  // "no response within deadline"

    // Formatted for logging:
    printf("error: %s\n", to_string(err).c_str());
    // → "response_lost[timeout]: no response within deadline"
    // → "notecard: {some device has no ProductUID configured}"
}
```

`Error` tells you *what* happened (and whether retrying is safe), `Cause` tells you *why*. See [docs/error-handling.md](docs/error-handling.md) for the full taxonomy.

Each request also carries a compile-time safety level (`ReadOnly`, `Idempotent`, `NonIdempotent`, `Destructive`) that your transport or retry logic can inspect to decide whether it's safe to retry a failed request.

---

## Wire Protocols

Header-only implementations of the Notecard serial and I2C protocols: `NotecardSerial` and `NotecardI2c`. These implement the same wire protocols as note-c's `n_serial.c` and `n_i2c.c` — CRC auto-detection, segmented TX/RX, retry logic, and auto-reset — so your application communicates using the standard framing Notecard expects.

Each protocol takes a thin platform HAL - a lightweight virtual interface for UART or I2C hardware access. Platform libraries like `note-cpp-arduino` and `note-cpp-zephyr` provide ready-made HAL implementations.

See [docs/transport.md](docs/transport.md) for the full HAL interface and implementation notes.

---

## JSON Backend

The Notecard uses JSON as its wire format — an implementation detail that `note-cpp` handles internally. Your application works with typed structs, not JSON strings. A default backend is provided that works out of the box.

If you have specific constraints (memory budget, existing JSON library, debugging needs), the backend is customizable. See [docs/json-backend.md](docs/json-backend.md) for when and why you might change it.

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

See [docs/](docs/README.md) for the full documentation index. Key pages:

- [Why note-cpp? (comparison with note-c)](docs/comparison.md)
- [Error handling](docs/error-handling.md)
- [Polymorphic APIs](docs/polymorphic-apis.md)
- [Duration units](docs/duration-units.md)
- [Body values and Note templates](docs/body-values.md)
- [JSON buffer builder](docs/json-builder.md)
- [Transport layer](docs/transport.md)
- [App orchestration (NTN, templates, sync)](docs/app-orchestration.md)
- [Project plan and status](docs/PLAN.md)
