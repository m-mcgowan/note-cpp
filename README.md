# note-cpp

Type-safe C++23 API for the [Blues Notecard](https://blues.com/notecard). Header-only, zero dependencies beyond the standard library.

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
   .set_product("com.example.app")
   .set_mode("periodic")
   .set_outbound(60)
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
   .set_file("sensors.qo")
   .set_body(r)
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
   .set_file("sensors.qo")
   .set_body(note::template_of<Readings>())
   .execute();
```

</td></tr>
</table>

With note-cpp, the compiler catches what note-c defers to runtime: wrong field names, wrong types, wrong enum values, missing required fields. And your IDE auto-completes every request, every field, and every response member.

## Table of Contents

- [JSON Buffer Builder](#json-buffer-builder)
- [Backend Interfaces](#backend-interfaces)
- [Notecard and Requests](#notecard-and-requests)
- [Generated API Types](#generated-api-types)
- [Body Values](#body-values)
- [Schemas and Templates](#schemas-and-templates)
- [Response Parsing](#response-parsing)
- [Error Handling](#error-handling)
- [Code Generation](#code-generation)
- [Building and Testing](#building-and-testing)

---

## JSON Buffer Builder

`JsonBuf<N>` writes JSON directly to a fixed-size `char` buffer. No allocations, no virtual dispatch — and fully `constexpr`, so when all values are compile-time constants, the entire JSON string is computed at build time.

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

**Compile-time evaluation** — when every value is a constant, the compiler resolves the entire string. `static_assert` proves the JSON is fully computed at build time:

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

Nested objects and arrays work too:

```cpp
constexpr auto note_add = [] {
    note::JsonBuf<256> b;
    b.add("req", "note.add");
    b.add("file", "sensors.qo");
    b.begin_object("body");
        b.add("temperature", 22.5);
        b.add("humidity", 60);
    b.end_object();
    b.close();
    return b;
}();

static_assert(note_add.view() ==
    R"({"req":"note.add","file":"sensors.qo","body":{"temperature":22.5,"humidity":60}})");
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

Use `json_const` when you have an explicit buffer size but want to enforce compile-time evaluation (compile error if any value is runtime, or if the buffer overflows):

```cpp
constexpr auto req = note::json_const([] {
    note::JsonBuf<64> b;
    b.add("req", "hub.sync");
    b.close();
    return b;
});

static_assert(req.view() == R"({"req":"hub.sync"})");
```

**Composable fragments** — build objects and arrays separately, then embed them:

```cpp
constexpr auto body = [] {
    auto b = note::JsonBuf<64>::object();
    b.add("temp", 22.5);
    b.add("humidity", 60);
    b.close();
    return b;
}();

constexpr auto files = [] {
    auto a = note::JsonBuf<64>::array();
    a.add("sensors.qo");
    a.add("config.db");
    a.close();
    return a;
}();

constexpr auto req = [] {
    note::JsonBuf<256> b;
    b.add("req", "note.add");
    b.add("body", body);
    b.add("files", files);
    b.close();
    return b;
}();

static_assert(req.view() ==
    R"({"req":"note.add","body":{"temp":22.5,"humidity":60},"files":["sensors.qo","config.db"]})");
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

## Backend Interfaces

note-cpp is transport-agnostic. You provide a JSON backend and a transport callable.

**JsonBackend** — wraps any JSON library (cJSON, nlohmann, RapidJSON, etc.):

```cpp
#include <note/json.hpp>

class MyJsonBackend : public note::JsonBackend {
    std::unique_ptr<note::JsonBuilder> create_builder() override;
    std::unique_ptr<note::JsonReader> parse_response(note::string_view json) override;
};
```

`JsonBuilder` has the same shape as `JsonBuf` — `add()`, `begin_object()`, `end_object()`, `begin_array()`, `end_array()` — but works through virtual dispatch against a live JSON library.

`JsonReader` provides typed accessors for reading responses:

```cpp
bool        get_bool(string_view key, bool def = false);
int32_t     get_int(string_view key, int32_t def = 0);
double      get_double(string_view key, double def = 0.0);
string_view get_string(string_view key, string_view def = {});
std::unique_ptr<JsonReader> get_object(string_view key);
```

---

## Notecard and Requests

`Notecard` is the central coordinator. It delegates JSON building to `JsonBackend` and transport to a callable — any function that takes a JSON string and returns a JSON string:

```cpp
#include <note/notecard.hpp>

MyJsonBackend backend;
note::Notecard nc(backend,
    [](note::string_view request, uint32_t timeout_ms) -> note::Result<std::string> {
        // Send request over serial/I2C, return response JSON string.
        return my_transport(request, timeout_ms);
    });
```

**Ad-hoc requests** — for quick one-offs or endpoints not yet in the generated API:

```cpp
auto result = nc.request("card.version");
if (result) {
    auto version = (*result)->get_string("version");
}
```

**Ad-hoc commands** (fire-and-forget):

```cpp
nc.command("hub.set", [](note::JsonBuilder& b) {
    b.add("product", "com.example.app");
    b.add("mode", "periodic");
});
```

---

## Generated API Types

74 request/response types are auto-generated from the [Notecard OpenAPI spec](notecard-api.openapi.json). Each is a plain aggregate struct with typed fields, chainable setters, and a `build()` method.

```cpp
#include <note/api_context.hpp>
#include <note/api/hub_set.hpp>

note::Api api(nc);

// Fluent factory chain
api.hubSet()
   .set_product("com.example.app")
   .set_mode("periodic")
   .set_outbound(60)
   .execute();

// Direct field assignment
auto req = api.hubSet();
req.mode = "continuous";
req.execute();

// Designated initializers (fields in declaration order)
api.execute(note::api::HubSet{.mode = "minimum", .product = "com.example.app"});
```

**Polymorphic endpoints** — overloaded Notecard requests (e.g. `note.get` with `delete:true` vs query) are separate types with their own fields, safety annotations, and response types:

```cpp
// Query (read-only, returns note content)
auto result = api.noteGet().query().set_file("data.qi").execute();

// Pop from queue (destructive, removes the note)
api.noteGet().delete_().set_file("requests.qi").execute();
```

**Fire-and-forget commands** — requests that don't need a response:

```cpp
auto req = api.hubSet();
req.product = "com.example.app";
req.command();  // sends "cmd" instead of "req"
```

**Compile-time enum validation**:

```cpp
auto req = api.hubSet();
req.mode = note::api::HubSet::validated_mode("periodic");  // OK
// req.mode = note::api::HubSet::validated_mode("typo");   // COMPILE ERROR
```

**Standalone usage** — request types work without the `Api` factory:

```cpp
note::api::CardVersion req;
auto result = req.execute(nc);
```

**Umbrella header** — include everything at once:

```cpp
#include <note/api_context.hpp>  // Api factory + all endpoints
// or
#include <note/api.hpp>          // all endpoint types without Api factory
```

---

## Body Values

Note bodies support three tiers, from simple to structured:

**Tier 1: Raw JSON string** — works everywhere, no parsing overhead:

```cpp
api.noteAdd()
    .set_file("sensors.qo")
    .set_body(R"({"temp":22.5})")
    .execute();
```

**Tier 2: Builder lambda** — structured body without a schema:

```cpp
api.noteAdd()
    .set_file("sensors.qo")
    .set_body(note::body([](note::JsonBuilder& b) {
        b.add("temp", 22.5);
        b.add("humidity", 60);
    }))
    .execute();
```

**Tier 3: Schema struct** — a C++ struct that serves triple duty: send, receive, and template registration. See [Schemas and Templates](#schemas-and-templates).

---

## Schemas and Templates

Define a struct once, use it to send data, receive data, and register Notecard templates.

**C++17** — use the `NOTE_BODY` macro:

```cpp
#include <note/body.hpp>

struct Readings {
    float temperature;
    int16_t humidity;
    NOTE_BODY(temperature, humidity)
};
```

**C++20** — plain aggregates work automatically via reflection (no macro needed):

```cpp
struct Readings {
    float temperature;
    int16_t humidity;
};
```

**Sending** — pass the struct as a body:

```cpp
Readings r{.temperature = 22.5f, .humidity = 60};
api.noteAdd().set_file("sensors.qo").set_body(r).execute();
// → {"req":"note.add","file":"sensors.qo","body":{"temperature":22.5,"humidity":60}}
```

**Template registration** — auto-generate Notecard type hints from the struct:

```cpp
api.noteTemplate().set()
    .set_file("sensors.qo")
    .set_body(note::template_of<Readings>())
    .execute();
// → {"req":"note.template","file":"sensors.qo","body":{"temperature":14.1,"humidity":11}}
//   (14.1 = TFLOAT, 11 = TINT16)
```

**Receiving** — parse a response body back into the struct:

```cpp
auto result = api.noteGet().query().set_file("data.qi").execute();
if (result) {
    // Typed struct
    auto r = result.body_as<Readings>();
    use(r.temperature, r.humidity);

    // Or ad-hoc field access
    if (auto* body = result.body()) {
        auto temp = body->get_double("temperature");
    }
}
```

---

## Error Handling

All operations return `Result<T>` (`std::expected<T, ErrorInfo>`):

```cpp
auto result = api.cardVersion().execute();
if (result) {
    note::string_view version = result.version;
} else {
    note::Error code = result.error().code;     // Timeout, Transport, Json, ...
    note::string_view msg = result.error().message;
}
```

**Safety classification** — each request carries a compile-time safety level for retry policy decisions:

```cpp
static_assert(note::api::CardVersion::safety == note::Safety::ReadOnly);
static_assert(note::is_safe_to_retry(note::Safety::Idempotent));
static_assert(!note::is_safe_to_retry(note::Safety::Destructive));
```

**API version gating** — restrict available fields to a specific firmware version:

```cpp
#define NOTE_API_VERSION NOTE_VERSION(7, 2, 0)
#include <note/api/hub_set.hpp>
// Fields added after 7.2.0 are compiled out
```

---

## Code Generation

The C++ types are generated from the OpenAPI spec:

```bash
pip install jinja2
python3 tools/codegen/generate.py notecard-api.openapi.json
```

This produces:
- 74 per-endpoint headers in `include/note/api/`
- Umbrella header `include/note/api.hpp`
- Api factory `include/note/api_context.hpp`
- 195 wire format tests in `tests/test_samples.cpp`

## Building and Testing

```bash
./ci.sh
```

Runs code generation, checks every header compiles independently, runs unit tests, and builds the smoke test. Requires a C++23 compiler (GCC 14+, Clang 18+, Apple Clang 15+).
