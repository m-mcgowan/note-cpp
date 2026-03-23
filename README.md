# note-cpp

[![CI](https://github.com/m-mcgowan/note-cpp/actions/workflows/ci.yml/badge.svg)](https://github.com/m-mcgowan/note-cpp/actions/workflows/ci.yml)

Type-safe C++ API for the [Blues Notecard](https://blues.com/notecard). Works with C++17, C++20, and C++23 — each version unlocks additional features. Header-only, zero dependencies beyond the standard library.

> **Community project.** Not affiliated with or supported by Blues Inc. Notecard is a trademark of Blues Inc.

## Why note-cpp?

The Notecard C API is *stringly-typed* — field names are strings, types are manual, typos compile fine and fail at runtime. `note-cpp` provides the Notecard API in a type-safe way, while also still allowing notes to be defined by the application, via type-safe structs. Requests and responses have typed fields and IDE auto-completion — misspelled field names, wrong types, and missing required fields are all compile errors. Targeting for the specific Notecard radio technologies and firmware version is also supported. [See the full comparison with note-c.](docs/comparison.md)

The API supports fluent setters

```cpp
// examples/getting_started.cpp#L193-L197

nc.hub.set()
   .product("com.example.app")
   .mode("periodic")
   .outbound(60)
   .execute();
```

as well as property assignment

```cpp
// examples/getting_started.cpp#L202-L206

auto req = nc.hub.set();
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
    nc.note.add().file("sensors.qo").body(r).execute();
}
```

Inline is fine too

```cpp
// examples/getting_started.cpp#L257-L260

nc.note.add()
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
    auto req = nc.note.add();
    req.file = "sensors.qo";
    req.body(r);
    req.execute();
}
```

## Developer Experience

note-cpp is designed for beginner to intermediate C++ developers. The API
should feel natural and expressive — code that reads like intent, not like
a fight with the type system. This isn't just a polish goal; it's a design
constraint that has shaped the library's architecture.

### Intent over protocol

Some Notecard APIs do very different things depending on which fields you
send. `note.get` can read a note by ID or pop one from a queue. `card.binary`
can check status or clear the buffer. In the C API, you have to know the
right field combination — the function name alone doesn't tell you what
will happen.

note-cpp gives these operations distinct, intent-revealing names:

```cpp
api.note.read("data.db").noteId("my-note").execute();  // read by ID
api.note.pop("requests.qi").execute();                  // pop from queue

api.binary.status().execute();                          // check binary state
api.binary.clear().execute();                           // clear the buffer
```

Each variant exposes only the fields that apply to that operation. Setting a
field that doesn't belong is a compile error, not a silent wire-level mistake.

### No boilerplate

Define a body struct once — use it to send data, receive data, and register
Notecard templates. No separate serialization code, no manual field mapping:

```cpp
struct Readings {
    float temperature;
    int16_t humidity;
    NOTE_FIELDS(temperature, humidity)  // optional on C++20
};

// Send
api.note.add().file("sensors.qo").body(Readings{.temperature = 22.5f, .humidity = 60}).execute();

// Receive
Readings data = result.bodyAs<Readings>();

// Register template (auto-generates type hints for compact storage)
api.note.templates().define("sensors.qo").body(note::template_of<Readings>()).execute();
```

### Meet the developer where they are

The API supports multiple idioms for the same operation — fluent chains,
direct assignment, and designated initializers. These aren't interchangeable
styles; they're different tools for different shapes of code:

```cpp
// Fluent — clear when configuring and executing in one statement
api.hub.set().product("com.example.app").mode("periodic").outbound(60).execute();

// Direct assignment — natural when fields come from your application's config
auto req = api.hub.set();
req.product  = app_config.product_uid;
req.mode     = app_config.sync_mode;
req.outbound = app_config.sync_interval;
req.execute();

// Conditional fields — set only what applies; unset fields are omitted from the request
auto req = api.hub.set();
req.product = app_config.product_uid;
req.mode    = app_config.sync_mode;
if (app_config.sync_mode == "continuous") {
    req.sync = true;  // only sent in continuous mode
}
req.execute();
```

Examples set the recommended patterns — the API supports alternatives so
developers can use what fits their context, while the examples guide best
practice for clarity and brevity.

### Natural syntax

**Typed fields behave like values.** Assign with `=`, read without
dereferencing. Under the hood they're thin optional wrappers, but the
user-facing syntax is just struct members:

```cpp
auto result = api.card.version().execute();
if (result) {
    auto version = result.version;   // not result->version or *result.version
}
```

**Errors are data, not exceptions.** Every operation returns a result. Check
it with `if`, inspect it with `.error()`. No try/catch, no surprise stack
unwinding, no hidden control flow — important on embedded where exceptions
are often disabled entirely.

**Units prevent mistakes, not just document them.** Duration fields use
distinct types (`Minutes`, `Seconds`, `Hours`) that convert implicitly in the
safe direction. Write `7_days` where `Minutes` is expected and the math
happens at compile time. Pass `Seconds` where `Minutes` is expected and
the compiler stops you.

**The compiler catches what it can.** Misspelled field names are compile
errors — there's no `"prodcut"` field on `hub.set`, so the compiler tells
you. Polymorphic variants expose only their valid fields, so setting a field
that doesn't apply to that operation won't compile.

On C++20, fields with a fixed set of valid values are validated transparently
at compile time. String literals are checked; runtime values pass through:

```cpp
req.mode = "periodic";     // ✓ validated at compile time
req.mode = "perioidc";     // ✗ compile error: hub.set: invalid value for 'mode'
req.mode = runtime_var;    // ✓ runtime value, no validation
```

Named constants are available for discoverability and IDE autocomplete:

```cpp
using mode = note::api::HubSet::mode_t;
req.mode = mode::periodic;      // autocomplete-friendly, typo-proof
req.mode = mode::continuous;
```

Target filtering warns (or errors in strict mode) when an API isn't available
on your Notecard SKU. The goal: catch mistakes before they reach the device.

### Where the complexity lives

The surface API is simple because the complexity is pushed elsewhere:

- **Code generation** — 74 request types are auto-generated from an OpenAPI
  spec. The generated code handles fluent setters, version gating, polymorphic
  dispatch, and JSON serialization. Users never edit or read it.
- **Internals** — Template metaprogramming, SFINAE, and `constexpr` machinery
  live in `detail` namespaces and implementation headers. They make the simple
  API possible, but don't leak into it.
- **Compile-time checks** — Target filtering, version gating, and enum
  validation use C++20 concepts and `consteval` when available, falling back
  gracefully on C++17. The user sees deprecation warnings or compile errors,
  not the machinery that produces them.

### Design principles

- **Clear over clever.** CRTP, expression templates, tag dispatch — these are
  tools for library internals, not for the API a developer types every day.
  If the API requires a C++ book to understand, it's a bug.
- **Templates only when they earn their place.** If a function takes
  `template<typename T>`, it should be for a clear reason (like
  `template_of<Readings>()` where the type itself is the input, not a value),
  not because the implementation was easier that way.
- **Namespaces, not macros.** `NOTE_FIELDS(...)` is the one macro, and it's
  optional on C++20. Everything else lives in the `note` namespace — no
  `#define`-driven configuration, no macro-based dispatch, no global
  pollution.

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
#include <note/api.hpp>
using namespace note::literals;
auto notecard = ....;               // we'll get to this later.
note::Api nc(notecard);

// Make requests. Fields are typed, IDE auto-completes everything.
nc.hub.set()
   .product("com.example.app")
   .mode("periodic")
   .outbound(60_mins)
   .execute();

// Read responses. Fields are named members, not strings.
auto rsp = nc.card.version().execute();
if (rsp) {
    auto version = rsp.version;   // string_view
    auto device  = rsp.device;    // string_view
} else {
    auto err = to_string(rsp.error());  // "transport: I2C NACK"
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

### Memory vs note-c

note-c uses `NOTE_C_LOW_MEM` to strip features on constrained platforms
(AVR, ESP8266, Cortex-M0+) — shorter error strings, no CRC validation,
no user-agent, smaller allocation chunks, `float` instead of `double`.

note-cpp avoids most of these tradeoffs structurally:

- **No heap allocation** — `BufferJsonBackend` uses stack buffers with
  template-controlled sizes. No `malloc`/`free` in steady state.
- **No error string variants** — `string_view` literals are short by
  design; the linker deduplicates identical strings.
- **No allocation chunk tuning** — transports reuse a member buffer
  (`std::string`); no per-call allocation after warmup.
- **Unused code eliminated** — `-ffunction-sections` + `--gc-sections`
  (default on Arduino/PlatformIO) removes unreferenced endpoints.

See the [migration guide](docs/migration-from-note-arduino.md#memory-and-binary-footprint)
for the full comparison table.

---

## Generated API Types

Request and response types covering all Notecard APIs (74 of them!) are auto-generated from the [Notecard OpenAPI spec](notecard-api.openapi.json). Each has typed fields, chainable setters, and an `execute()` method. Fields support fluent chaining, direct assignment, and designated initializers.

```cpp
nc.hub.set()
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

Include everything with `#include <note/api.hpp>`.

---

## Target Filtering

When targeting a specific Notecard product, the `Api` constructor accepts a target that provides compile-time feedback on API compatibility (C++20). Unsupported APIs produce deprecation warnings, or compile errors in strict mode. Built-in targets include `Product::WiFi`, `Product::Cell`, `Product::LoRa`, and `Product::Skylo`, and you can compose custom targets with additional RATs (e.g. `Product::Cell + Rat::Ntn`).

```cpp
note::Api<note::Product::WiFi> nc(notecard);
nc.card.sleep();  // OK: card.sleep supports WiFi
nc.hub.set();     // OK: universal
```

Each API type carries `static constexpr Skus skus` for introspection. See [examples/target_filtering.cpp](examples/target_filtering.cpp).

---

## Polymorphic APIs

Some Notecard APIs behave differently depending on which fields you send. In `note-c` these share a single function — you pass the right combination of fields and hope you didn't set one that's irrelevant or wrong. In `note-cpp`, each behavior is a **distinct type** with only the fields that apply, its own response type, and a compile-time safety level:

```cpp
// Read a Note by ID (ReadOnly — safe to retry on failure)
auto rsp = nc.note.read("data.db").noteId("my-note").execute();

// Pop from queue (Destructive — not safe to retry blindly)
auto rsp = nc.note.pop("requests.qi").execute();

// card.location.mode — fixed() accepts lat/lon, get() doesn't
nc.card.locationMode().fixed()
    .lat(42.565).lon(-70.783)   // lat/lon only exist on fixed()
    .execute();

nc.card.locationMode().get().execute();       // no lat/lon fields to misuse
```

Each variant exposes only the fields the Notecard expects for that operation — setting a field that doesn't apply is a compile error, not a silent wire-level mistake.

The same pattern applies to `card.binary`, `card.contact`, `card.location.mode`, `card.temp`, `note.template`, and others. See [docs/polymorphic-apis.md](docs/polymorphic-apis.md) for the full list.

---

## API Name Mapping

Notecard request names map to C++ as follows:

- The **first segment** of the request name becomes a **resource group** property on `Api` — `nc.card`, `nc.hub`, `nc.note`, etc.
- The **remaining segments** become the group method in camelCase — `card.location.mode` → `nc.card.locationMode()`, `hub.sync.status` → `nc.hub.syncStatus()`.
- For single-segment requests (`web`, `file`), the request name is the group: `web.get` → `nc.web.get()`.

Two endpoints use plural group method names to avoid C++ keywords:

| Wire name | C++ group method | Reason |
|-----------|-----------------|--------|
| `env.default` | `nc.env.defaults()` | `default` is a C++ keyword |
| `note.template` | `nc.note.templates()` | `template` is a C++ keyword |

### Intent method names

For polymorphic APIs, the method names on the factory struct are **intent-based** — describing what the operation *does*, not the underlying HTTP verb. Old verb-based names (`get()`, `set()`, `delete_()`) are kept as `[[deprecated]]` aliases.

| Notecard request | C++ method |
|-----------------|------------|
| `card.binary` | `.status()` · `.clear()` |
| `card.power` | `.read()` · `.configure()` · `.reset()` |
| `card.temp` | `.read()` · `.configure()` · `.stop()` |
| `card.voltage` | `.read()` · `.configure()` |
| `card.wireless.penalty` | `.check()` · `.override_()` · `.clear()` |
| `env.default` | `.set(name, text)` · `.remove(name)` |
| `note.changes` | `.peek()` · `.pop(file)` |
| `note.get` | `.read()` · `.pop()` |
| `note.template` | `.define(file)` · `.remove(file)` |
| `card.location.mode` | `.get()` · `.fixed()` · `.remove()` |

### Convenience shortcuts

Frequently-used operations have shorthand methods that pre-fill required parameters, named for intent rather than the endpoint:

```cpp
nc.note.read("data.db").noteId("my-note").execute();   // note.get (read by ID)
nc.note.pop("requests.qi").execute();                   // note.get (pop from queue)
nc.note.remove("data.db", "my-note").execute();         // note.delete
nc.env.setDefault("var", "value").execute();            // env.default set
nc.env.clearDefault("var").execute();                   // env.default remove
nc.binary.status().execute();                           // card.binary status (flat alias)
nc.binary.clear().execute();                            // card.binary clear (flat alias)
```

On C++20, shorthand methods also accept a named-field struct:

```cpp
nc.note.remove({.file = "data.db", .noteId = "my-note"}).execute();
nc.env.setDefault({.name = "var", .text = "value"}).execute();
```

### Renamed properties

Some properties are renamed in C++ to avoid keyword conflicts or improve clarity:

| Wire name | C++ name | Reason |
|-----------|----------|--------|
| `note` (note ID field) | `noteId` | `note` clashes with the `note` namespace; clarifies meaning |
| `delete` (boolean field) | `delete_` | `delete` is a reserved C++ keyword |

### Delete aliases

All `*.delete` endpoints expose a `remove()` alias. Where the endpoint has required parameters, both forms accept them:

```cpp
nc.note.remove("data.db", "my-note").execute();   // preferred
nc.note.delete_("data.db", "my-note").execute();  // direct (deprecated)
nc.file.remove().execute();                        // file.delete (no required params)
```

---

## Body Values

Note bodies support three tiers: **raw JSON strings**, **builder lambdas**, and **typed structs**. The struct approach is the most powerful — a single struct serves as builder, parser, and definition for Note template registration. See [Body Structs and Note Templates](#body-structs-and-note-templates).

```cpp
Readings r{.temperature = 22.5f, .humidity = 60};
nc.note.add().file("sensors.qo").body(r).execute();
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
    nc.note.add().file("sensors.qo").body(r).execute();
}
```

Register a template (auto-generates type hints `14.1` = TFLOAT32, `11` = TINT16):

```cpp
// examples/getting_started.cpp#L277-L279

nc.note.templates().define("sensors.qo")
    .body(note::template_of<Readings>())
    .execute();
```

Parse response body back into the struct:

```cpp
// examples/getting_started.cpp#L283-L291

{
    auto rsp = nc.note.read("data.qi").execute();
    if (rsp) {
        Readings data = rsp.bodyAs<Readings>();
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
nc.hub.set()
    .outbound(15_mins)           // Minutes literal
    .inbound(7_days)             // Days → Minutes (10080 on the wire)
    .execute();

// card.attn seconds field accepts Minutes/Hours too
nc.card.attn()
    .seconds(5_mins)             // Minutes → Seconds (300 on the wire)
    .execute();

// card.sleep — long sleep expressed naturally
nc.card.sleep()
    .seconds(12_hours)           // Hours → Seconds (43200 on the wire)
    .execute();

// Compile-time safety — wrong direction is a type error:
// nc.hub.set().outbound(300_s);    // error: Seconds ≠ Minutes
```

**Voltage-variable sync** — adapt sync frequency to the Notecard's supply voltage. A builder constructs the semicolon-delimited string safely:

```cpp
auto req = nc.hub.set();
req.mode = "periodic";
req.voutbound.usb(5).high(15).normal(60).low(240).dead(0);
req.vinbound.usb(5).high(30).normal(120).low(1440).dead(0);
req.execute();
// produces: "voutbound":"usb:5;high:15;normal:60;low:240;dead:0"
```

Only levels you set are emitted — `.voutbound.usb(5).normal(60)` produces `"usb:5;normal:60"`.

**Comma-separated flags** — fields like `card.attn` mode accept a comma-delimited set of flags. Named methods provide compile-time safety:

```cpp
auto req = nc.card.attn();
req.mode.arm().connected().files();       // chainable named methods
req.execute();
// produces: "mode":"arm,connected,files"

// Or use flag constants with operator|
using namespace note::attn;
req.mode = arm | connected | files;

// Raw strings still work for dynamic values
req.mode = "arm,connected";
```

Flag constants live in `note::attn`, `note::triangulate`, etc.

See [examples/hub-configuration/](examples/hub-configuration/) for more.

---

## Error Handling

All operations return a result that is truthy on success. On failure, `error()` provides a structured `ErrorInfo` with an error code and message. `to_string()` formats it for logging.

```cpp
auto result = nc.card.version().execute();
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

### Binary transfers

Some Notecard APIs (`card.binary.get`, `card.binary.put`, `dfu.get`) transfer
raw binary data over the same wire. The Notecard uses COBS (Consistent Overhead
Byte Stuffing) to frame this data — an encoding detail that belongs to the
transport, not the application.

The intended API for these requests is buffer-based: you provide a source or
destination buffer and a byte count; the library handles COBS encode/decode
internally. The `cobs` fields visible on the request/response structs are
exposed for diagnostics, but they are not part of the normal call pattern —
you should not need to compute or pass encoded sizes.

> **Note:** The buffer-based `execute()` overloads for binary requests are
> planned but not yet implemented. See `notecard.hpp` for the design note.

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

- [Migrating from note-arduino](docs/migration-from-note-arduino.md)
- [Why note-cpp? (comparison with note-c)](docs/comparison.md)
- [Error handling](docs/error-handling.md)
- [Polymorphic APIs](docs/polymorphic-apis.md)
- [Duration units](docs/duration-units.md)
- [Body values and Note templates](docs/body-values.md)
- [JSON buffer builder](docs/json-builder.md)
- [Transport layer](docs/transport.md)
- [App orchestration (NTN, templates, sync)](docs/app-orchestration.md)
- [Project plan and status](docs/PLAN.md)
