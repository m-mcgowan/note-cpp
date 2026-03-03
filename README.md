# note-cpp

[![CI](https://github.com/m-mcgowan/note-cpp/actions/workflows/ci.yml/badge.svg)](https://github.com/m-mcgowan/note-cpp/actions/workflows/ci.yml)

Type-safe C++23 API for the [Blues Notecard](https://blues.com/notecard). Header-only, zero dependencies beyond the standard library.

> **Community project.** Not affiliated with or supported by Blues Inc. Notecard is a trademark of Blues Inc.

## Why note-cpp?

The Notecard C API works, but every request is a bag of untyped strings and numbers. Typos compile fine and fail at runtime. There's nothing to auto-complete.

<table>
<tr><th>note-c</th><th>note-cpp</th></tr>
<tr><td>

```c
// Configure product — field names are
// strings, types are manual, no IDE help.
J *req = NoteNewRequest("hub.set");
JAddStringToObject(req, "product",
    "com.example.app");
JAddStringToObject(req, "mode", "periodic");
JAddNumberToObject(req, "outbound", 60);
NoteRequest(req);
```

</td><td>

```cpp
// Every field is a named member.
// IDE auto-completes after the dot.
api.hubSet()
   .product("com.example.app")
   .mode("periodic")
   .outbound(60)
   .execute();
```

</td></tr>
<tr><td>

```c
// Send a note — body is manual J* tree.
J *req = NoteNewRequest("note.add");
JAddStringToObject(req, "file", "sensors.qo");
J *body = JAddObjectToObject(req, "body");
JAddNumberToObject(body, "temp", 22.5);
JAddNumberToObject(body, "humidity", 60);
NoteRequest(req);
```

</td><td>

```cpp
// Body from a typed struct — same type
// registers templates and parses responses.
Readings r{.temperature = 22.5f, .humidity = 60};
api.noteAdd()
   .file("sensors.qo")
   .body(r)
   .execute();
```

</td></tr>
<tr><td>

```c
// Read response — stringly-typed, no
// compiler help if you misspell a field.
J *rsp = NoteRequestResponse(
    NoteNewRequest("card.version"));
char *ver = JGetString(rsp, "verison"); // typo!
char *dev = JGetString(rsp, "device");
NoteDeleteResponse(rsp);
```

</td><td>

```cpp
// Response is a typed struct — misspelled
// fields won't compile. Dot access, not arrow.
auto r = api.cardVersion().execute();
if (r) {
    auto ver = r.version; // typo = compile error
    auto dev = r.device;
}
```

</td></tr>
<tr><td>

```c
// Register template — magic numbers
// for type hints, easy to get wrong.
J *req = NoteNewRequest("note.template");
JAddStringToObject(req, "file", "sensors.qo");
J *body = JAddObjectToObject(req, "body");
JAddNumberToObject(body, "temperature", 14.1);
JAddNumberToObject(body, "humidity", 11);
NoteRequest(req);
```

</td><td>

```cpp
// Same Readings struct auto-generates
// the correct Notecard type hints.
api.noteTemplate().set()
   .file("sensors.qo")
   .body(note::template_of<Readings>())
   .execute();
```

</td></tr>
</table>

With note-cpp, the compiler catches what note-c defers to runtime: wrong field names, wrong types, wrong enum values, missing required fields. And your IDE auto-completes every request, every field, and every response member.

---

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
note::Api api(nc);

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

Build and run the example:

```bash
c++ -std=c++2b -I include examples/getting_started.cpp && ./a.out
```

## Table of Contents

- [Generated API Types](#generated-api-types)
- [Polymorphic Endpoints](#polymorphic-endpoints)
- [Body Values](#body-values)
- [Schemas and Templates](#schemas-and-templates)
- [Error Handling](#error-handling)
- [Backend Interfaces](#backend-interfaces)
- [Transport Layer](#transport-layer)
- [Notecard and Ad-hoc Requests](#notecard-and-ad-hoc-requests)
- [JSON Buffer Builder](#json-buffer-builder)
- [Code Generation](#code-generation)
- [Building and Testing](#building-and-testing)

---

## Generated API Types

74 request/response types are auto-generated from the [Notecard OpenAPI spec](notecard-api.openapi.json). Each has typed fields, chainable setters, and an `execute()` method.

Every field supports three access patterns:

```cpp
#include <note/api_context.hpp>

note::Api api(nc);

// 1. Fluent chain — functor call returns the parent, enabling chaining
api.hubSet()
   .product("com.example.app")
   .mode("periodic")
   .outbound(60)
   .execute();

// 2. Direct field assignment — assign directly, read back with * or implicit conversion
note::api::HubSet req;
req.mode = "continuous";
req.outbound = 30;
if (req.mode) { /* has_value() */ }
auto m = *req.mode;  // string_view

// 3. Designated initializers — aggregate init, all fields optional
api.execute(note::api::HubSet{.mode = "minimum", .outbound = 120});
```

All three patterns are equivalent — pick the one that fits the context.

**Undocumented properties** — if you need to send a field that isn't in the spec yet, use `extra()`:

```cpp
req.mode("periodic").extra("exp_feature", true).extra("count", 42);
```

**String key access** — `operator[]` routes known keys to their typed field and unknown keys to the extras buffer. Useful when the key is a reserved C++ word or comes from a variable:

```cpp
req["mode"] = note::string_view("periodic");   // routes to typed field
req["delete"] = true;                          // reserved word — no conflict
req["undocumented"] = int32_t{7};             // unknown key → extras buffer
```

**Fire-and-forget commands** — send `"cmd"` instead of `"req"` when no response is needed:

```cpp
api.hubSet().product("com.example.app").command();
```

**Compile-time enum validation**:

```cpp
req.mode = note::api::HubSet::validatedMode("periodic");  // OK
// req.mode = note::api::HubSet::validatedMode("typo");   // COMPILE ERROR
```

**Umbrella header** — include all 74 endpoints and the `Api` factory at once:

```cpp
#include <note/api_context.hpp>
```

---

## Polymorphic Endpoints

Some Notecard endpoints behave differently depending on which fields you send. note-cpp models these as **named sub-operations** — each has its own typed fields and response type.

There are two equivalent access styles:

| Style | Example | When to use |
|-------|---------|-------------|
| **Endpoint-first** | `api.noteGet().get()` | Mirrors the Notecard API reference structure |
| **Action-first** | `api.getNoteGet()` | Verb-first; easier to discover by typing `api.get...` |

### note.get

```cpp
#include <note/api/note_get.hpp>

// Read a note (non-destructive)
auto result = api.noteGet().get().file("data.qi").execute();
//           — or —
auto result = api.getNoteGet().file("data.qi").execute();

if (result) {
    auto body = result.bodyAs<Readings>();
}

// Pop from the inbound queue (destructive — consumes the note)
api.noteGet().delete_().file("requests.qi").execute();
//           — or —
api.deleteNoteGet().file("requests.qi").execute();
```

### card.location.mode

```cpp
#include <note/api/card_location_mode.hpp>

// Configure
api.cardLocationMode().set().mode("periodic").seconds(300).execute();
//                   — or —
api.setCardLocationMode().mode("periodic").seconds(300).execute();

// Query current mode
auto result = api.cardLocationMode().get().execute();
//                                  — or —
auto result = api.getCardLocationMode().execute();

// Reset to defaults
api.cardLocationMode().delete_().execute();
//                    — or —
api.deleteCardLocationMode().execute();
```

### note.template

```cpp
#include <note/api/note_template.hpp>

// Register a template
api.noteTemplate().set().file("sensors.qo").body(note::template_of<Readings>()).execute();
//                — or —
api.setNoteTemplate().file("sensors.qo").body(note::template_of<Readings>()).execute();

// Delete a template
api.noteTemplate().delete_().file("sensors.qo").execute();
```

### Other polymorphic endpoints

The same pattern applies to `card.binary`, `card.contact`, `card.power`, `card.temp`, and several others. Each exposes both endpoint-first (e.g. `api.cardTemp().get()`) and action-first (e.g. `api.getCardTemp()`) access.

---

## Body Values

Note bodies support three tiers, from simple to structured:

**Tier 1: Raw JSON string** — works everywhere, no parsing overhead:

```cpp
api.noteAdd()
    .file("sensors.qo")
    .body(R"({"temp":22.5})")
    .execute();
```

**Tier 2: Builder lambda** — structured body without defining a struct:

```cpp
api.noteAdd()
    .file("sensors.qo")
    .body(note::body([](note::JsonBuilder& b) {
        b.add("temp", 22.5);
        b.add("humidity", int32_t{60});
    }))
    .execute();
```

**Tier 3: Schema struct** — a C++ struct that serves triple duty: send, receive, and template registration. See [Schemas and Templates](#schemas-and-templates).

---

## Schemas and Templates

Define a struct once, use it to send data, receive data, and register Notecard templates. Add the `NOTE_BODY` macro listing the fields:

```cpp
#include <note/body.hpp>

struct Readings {
    float temperature;
    int16_t humidity;
    NOTE_BODY(temperature, humidity)
};
```

**Sending** — pass the struct as a body:

```cpp
Readings r{.temperature = 22.5f, .humidity = 60};
api.noteAdd().file("sensors.qo").body(r).execute();
// -> {"req":"note.add","file":"sensors.qo","body":{"temperature":22.5,"humidity":60}}
```

**Template registration** — auto-generate Notecard type hints from the struct:

```cpp
api.noteTemplate().set()
    .file("sensors.qo")
    .body(note::template_of<Readings>())
    .execute();
// -> {"req":"note.template","file":"sensors.qo","body":{"temperature":14.1,"humidity":11}}
//    (14.1 = TFLOAT32, 11 = TINT16 — the Notecard's type hint values)
```

**Receiving** — parse a response body back into the struct:

```cpp
auto result = api.noteGet().get().file("data.qi").execute();
if (result) {
    // Typed struct — NOTE_BODY or plain aggregate (C++20)
    auto r = result.bodyAs<Readings>();
    use(r.temperature, r.humidity);

    // Or ad-hoc field access via the JSON reader
    if (auto* body = result.body()) {
        auto temp = body->get_double("temperature");
    }
}
```

---

## Error Handling

All operations return a result that is truthy on success:

```cpp
auto result = api.cardVersion().execute();
if (result) {
    // Access response fields directly with dot notation
    note::string_view version = result.version;
    note::string_view device  = result.device;
} else {
    // Error details
    note::Error      code = result.error().code;     // Timeout, Transport, Json, Protocol, ...
    note::string_view msg = result.error().message;
}
```

**Safety classification** — each request carries a compile-time safety level for retry decisions:

```cpp
static_assert(note::api::CardVersion::safety == note::Safety::ReadOnly);
static_assert(note::is_safe_to_retry(note::Safety::Idempotent));
static_assert(!note::is_safe_to_retry(note::Safety::Destructive));
```

**API version gating** — restrict available fields to a specific Notecard firmware version. Fields introduced after the specified version are compiled out, so you won't accidentally use features your firmware doesn't support:

```cpp
#define NOTE_API_VERSION NOTE_VERSION(7, 2, 0)
#include <note/api/hub_set.hpp>
// Fields added after firmware 7.2.0 are not available
```

If `NOTE_API_VERSION` is not defined, all fields are available (defaults to the latest).

---

## Backend Interfaces

note-cpp is transport-agnostic. You provide two things:

1. A **`JsonBackend`** — wraps your JSON library (cJSON, nlohmann, RapidJSON, etc.)
2. A **transport callable** — sends a JSON string and returns a JSON string

**JsonBackend** — implement two methods:

```cpp
#include <note/json.hpp>

class MyJsonBackend : public note::JsonBackend {
    // Build a JSON request string
    std::unique_ptr<note::JsonBuilder> create_builder() override;

    // Parse a JSON response string
    std::unique_ptr<note::JsonReader> parse_response(note::string_view json) override;
};
```

`JsonBuilder` has simple methods: `add(key, value)` for each type, plus `begin_object()`/`end_object()` and `begin_array()`/`end_array()` for nesting.

`JsonReader` provides typed accessors:

```cpp
bool        get_bool(string_view key, bool def = false);
int32_t     get_int(string_view key, int32_t def = 0);
double      get_double(string_view key, double def = 0.0);
string_view get_string(string_view key, string_view def = {});
std::unique_ptr<JsonReader> get_object(string_view key);
```

**Transport** — any callable that takes a request string and returns a response string:

```cpp
note::Notecard nc(backend,
    [](note::string_view request, uint32_t timeout_ms) -> note::Result<std::string> {
        // Send request over serial/I2C, return response JSON.
        return my_transport(request, timeout_ms);
    });
```

See [examples/getting_started.cpp](examples/getting_started.cpp) for a complete working example with a mock backend.

---

## Transport Layer

note-cpp ships complete, header-only implementations of both Notecard wire protocols:

| Header | Class | Ported from |
|---|---|---|
| `note/transport/serial.hpp` | `NotecardSerial` | note-c `n_serial.c` |
| `note/transport/i2c.hpp` | `NotecardI2c` | note-c `n_i2c.c` |

Both handle CRC auto-detection, segmented TX, retry logic, and auto-reset on first use. Each takes a platform HAL — a small virtual interface you implement for your target (UART read/write, I2C read/write, millis, delay).

See **[docs/transport.md](docs/transport.md)** for the full HAL interface, callback variants, protocol constants, and implementation notes for both transports.

Platform-specific HAL implementations (Arduino Wire, Zephyr, ESP-IDF, Linux serial) live in separate repos that depend on note-cpp.

---

## Notecard and Ad-hoc Requests

The `Api` factory (shown above) is the recommended way to make requests. For endpoints not yet in the generated API, or for quick one-offs, use ad-hoc methods on `Notecard` directly:

```cpp
// Ad-hoc request — returns a JsonReader for the response
auto result = nc.request("card.version");
if (result) {
    auto version = (*result)->get_string("version");
}

// Ad-hoc request with fields
nc.request("hub.set", [](note::JsonBuilder& b) {
    b.add("product", "com.example.app");
    b.add("mode", "periodic");
});

// Fire-and-forget command
nc.command("hub.set", [](note::JsonBuilder& b) {
    b.add("product", "com.example.app");
});
```

---

## JSON Buffer Builder

`JsonBuf<N>` writes JSON into a fixed-size `char` buffer with no allocations and no virtual dispatch. It's fully `constexpr` — when all values are compile-time constants, the JSON string is computed at build time.

This is useful for building static request strings or body payloads without a JSON library. Most users won't need it — the `Api` layer handles JSON building automatically.

```cpp
#include <note/json_buf.hpp>

// Runtime: build JSON into a stack buffer
note::JsonBuf<256> b;
b.add("req", "note.add");
b.add("file", "sensors.qo");
b.begin_object("body");
    b.add("temperature", 22.5);
    b.add("humidity", 60);
b.end_object();
b.close();

send(b.data(), b.size());   // b.view() returns a string_view
```

**Compile-time evaluation** — when every value is a constant, the compiler resolves the entire string:

```cpp
constexpr auto hub_set = [] {
    note::JsonBuf<128> b;
    b.add("req", "hub.set");
    b.add("mode", "periodic");
    b.add("outbound", 60);
    b.close();
    return b;
}();

// Verified at compile time — build fails if the JSON doesn't match:
static_assert(hub_set.view() == R"({"req":"hub.set","mode":"periodic","outbound":60})");
```

**Auto-sized** — use `note::json` when you don't want to pick a buffer size. The compiler measures the output and allocates exactly the right amount:

```cpp
constexpr auto req = note::json<[](auto& b) {
    b.add("req", "hub.set");
    b.add("mode", "periodic");
    b.add("outbound", 60);
    b.close();
}>();

static_assert(req.view() == R"({"req":"hub.set","mode":"periodic","outbound":60})");
```

**Overflow detection** — follows the `snprintf` convention. On overflow, `size()` reports the number of bytes that *would have been* written:

```cpp
note::JsonBuf<16> b;
b.add("req", "hub.set");
b.add("mode", "periodic");
b.close();

if (!b) {
    // b.size()     == 35  (bytes needed)
    // b.capacity() == 16  (buffer size)
}
```

---

## Code Generation

The 74 endpoint types are generated from the OpenAPI spec:

```bash
pip install jinja2
python3 tools/codegen/generate.py notecard-api.openapi.json
```

This produces per-endpoint headers in `include/note/api/`, umbrella headers, the Api factory, and 195 wire-format tests.

## Building and Testing

```bash
./ci.sh                  # default compiler
./ci.sh --all-compilers  # all locally installed compilers
./ci.sh --coverage       # coverage report (requires GCC 13+ and lcov 2.x)
```

Runs code generation, checks every header compiles independently, runs unit tests, and builds the examples. Requires a C++23 compiler (GCC 13+, Clang 18+, Apple Clang 15+).

Coverage generates `coverage/html/index.html`. Current results: ~98.5% lines, ~99.9% functions, ~98.9% branches. See [docs/coverage.md](docs/coverage.md) for toolchain requirements and details.
