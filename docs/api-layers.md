# API Layers

First and foremost, `note-cpp` is designed for a great developer experience
from the outset. The primary
API aims to read like plain English — no templates, no angle brackets,
no lambdas or function pointers. You call methods, set fields, and execute. The complexity lives
in the internals; the surface you interact with is deliberately simple.

Underneath that surface are progressively lower layers for increasingly advanced use
cases. Each inner layer trades some simplicity for flexibility — introducing
lambdas for request building and response parsing, then at the bottom, a string-based transaction API. The typed API
is what most users should use, but the lower layers are there letting you drop down when needed.


```
┌──────────────────────────────────────────────────────┐
│  Typed API                                           │
│                                                      │
│   Guided (operations):                               │
│     nc.card.attn().arm().connected().seconds(120)    │
│                                                      │
│   Unguided (full field access):                      │
│     CardAttn::Request req;                           │
│     req.mode = "arm,connected,some_new_mode";        │
│                                                      │
│  ┌──────────────────────────────────────────────┐    │
│  │  Lambda Request Builder                      │    │
│  │   nc.request("card.attn", [](auto& b)        │    │
│  │     { b.add("customproperty","value"); });   │    │
│  │                                              │    │
│  │  ┌──────────────────────────────────────┐    │    │
│  │  │  Raw JSON                            │    │    │
│  │  │   nc.transact(json_string)           │    │    │
│  │  └──────────────────────────────────────┘    │    │
│  └──────────────────────────────────────────────┘    │
└──────────────────────────────────────────────────────┘
```

All three layers share the same transport and can be mixed freely in the
same firmware. Use the outermost layer that fits your needs.

## Typed API

The typed API is the primary interface and the one most developers should
use exclusively. There are no templates, no angle brackets, and no
lambdas — just methods, fields, and `execute()`. The API mirrors the
Notecard's JSON structure using plain C++ naming, so if you know the
Notecard API, you already know how to use it.

### Guided Requests (Focused APIs)

Many Notecard endpoints do different things depending on which fields you
send. For example, `card.attn` can arm, disarm, query, sleep, or rearm —
all via the same JSON `req` string. The typed API splits these into named
**operations**, each exposing only the fields that apply:

```cpp
// Arm ATTN for file changes with 2-minute timeout
nc.card.attn().arm()
    .files()
    .connected()
    .seconds(120_s)
    .execute();

// Query what triggered ATTN
auto q = nc.card.attn().query().execute();

// Read temperature
auto r = nc.card.temp().read().execute();
if (r) Serial.println(r.value);

// Pop a Note from a queue
auto r = nc.note.pop("requests.qi").execute();
```

Notice what's absent: no `<template>` parameters, no `std::function`, no
JSON key strings, no casts or other advanced or low-level constructs.
You interact with named methods and typed fields.
Autocomplete shows you what's available — `arm()` shows arming
fields, `query()` shows query fields. Setting a field that doesn't belong
is a compile error, not a runtime surprise on a device in the field.

Guided requests give you:
- **Compile-time field validation** — misspelled or misplaced fields are compile errors.
- **Focused field surface** — only the fields relevant to the operation are visible in the request and response.
- **Safety classification** — each operation carries `ReadOnly`, `Idempotent`, or `Destructive` for retry decisions.
- **Target gating** (C++20) — compile warnings or errors when an endpoint is not available on your Notecard SKU.

The examples throughout this page use fluent syntax, but every request
also supports method calls (`req.mode("periodic")`), direct field
assignment (`req.mode = "periodic"`), and designated initializers (C++20).
See [API Calling Patterns](api-patterns.md) for all styles. See
[Focused APIs](focused-apis.md) for the full design.

### Unguided Requests (Full Field Access)

Operation methods are built on top of typed request structs — they pre-set
the fields needed by the Notecard for the operation being performed and
include only the visible surface needed. The underlying general request
type has all fields. You can access it directly when you need a field
combination the library doesn't guide you towards:

```cpp
// The typed API doesn't know about "some_future_mode" yet,
// but the full request type lets you pass any string:
note::api::CardAttn::Request req;
req.mode = "arm,connected,some_future_mode";
req.seconds = 120;
nc.execute(req);
```

There are still no lambdas and no templates — just struct field assignment.
You get the same typed response and the same retry behavior. What you lose
is the library's guidance: all fields are visible, and it is up to you to
pick the right combination. The Notecard still validates at runtime.

Use unguided requests when:
- New Notecard firmware adds something the typed API does not model yet.
- You need a field combination that spans multiple operations.
- You are prototyping and want to iterate quickly on field combinations.

For fields that are not in the schema at all, the "extras" feature `operator[]` lets you set
arbitrary key-value pairs on any request. See
[Ad-Hoc Fields](api-patterns.md#ad-hoc-fields-operator) in the API
Calling Patterns guide.

## Lambda Request Builder

The lambda request builder is the first layer that makes use of lambdas.
It is targeted at advanced use cases such as migrating custom JSON structures from existing code, avoiding
the need to declare struct types for your custom note bodies - although we feel the effort is worth it,
especially when those structs already exist.

The lambda request builder lets you build JSON by hand without any
generated types. This is useful for entirely new request types or as a
familiar entry point for developers migrating from `note-c`, where all
requests are hand-built JSON. The typed API internally uses lambda
builders to build requests and parse responses.

```cpp
// Ad-hoc request with manual JSON building
auto result = nc.request("card.attn", [](note::JsonBuilder& b) {
    b.add("mode", "arm,connected");
    b.add("seconds", 120);
});
if (result) {
    auto& reader = result.reader();
    bool set = reader.get_bool("set");
}

// Fire-and-forget (sends "cmd" instead of "req")
nc.command("hub.sync");
```

There is no type safety on either end — you build the request and parse
the response manually. This provides maximum flexibility with minimum
guardrails. See the
[getting started example](../examples/stdcpp/getting-started.cpp) (section
1, "Ad-hoc requests") for building requests and reading response fields
by name, and the [sending notes example](../examples/stdcpp/sending-notes/)
for building and parsing custom bodies (lambda, typed struct, and
`.into()` patterns).

### Migration from note-c

If you are coming from `note-c` or `note-arduino`, the lambda request
builder is the easiest starting point. Your existing mental model of
"build a JSON request, send it, parse the response" maps directly:

```cpp
// note-c:
J* req = NoteNewRequest("hub.set");
JAddStringToObject(req, "product", "com.example.app");
JAddStringToObject(req, "mode", "periodic");
NoteRequest(req);

// note-cpp lambda request builder — same structure, different syntax:
nc.request("hub.set", [](note::JsonBuilder& b) {
    b.add("product", "com.example.app");
    b.add("mode", "periodic");
});

// or use the typed API:
nc.hub.set().product("com.example.app").mode("periodic").execute();
```

You can migrate one request at a time. All styles coexist in the same
firmware, sharing the same serial or I2C connection.

## Raw JSON

Raw JSON is the lowest level — raw strings in, raw strings out. It is
intended for protocol-level work and highly constrained environments.

Pass a pre-built JSON string directly to the Notecard. The response comes
back as a string you parse yourself.

```cpp
// Caller-owned buffer for the response
char buf[256];
auto rsp = nc.transact(R"({"req":"card.version"})", buf);
if (rsp) {
    // *rsp is a string_view into buf containing the JSON response
}

// Heap-allocated (OwnedBuffer, freed on scope exit)
auto rsp = nc.transact(R"({"req":"card.version"})");
```

To extract fields from a raw response without pulling in the SAX
parser, pair `transact_raw` with `note::JsonView` — substring
lookups against the buffered response. This skips ~8 KB of flash
on AVR compared to `transact_dispatch` + a `JsonSink`:

```cpp
#include <note/json_view.hpp>

char buf[128];
auto body = note::JsonView(
    nc.stack().transport.transact_raw(req.view(), buf, sizeof(buf), 10000)
).object("body");

float temp   = body.get_float("temp");
int32_t hum  = body.get_int  ("humidity");
```

See the [Arduino guide](platforms/arduino/guide.md#binary-size-comparison)
for the full flash/RAM comparison between the SAX-sink and
`JsonView` scan approaches.

For requests with runtime values, `JsonBuf` builds the JSON into a
fixed-size buffer with no heap allocation:

```cpp
float temperature = read_sensor();

note::JsonBuf<128> req;
req.add("req", "note.add");
req.add("file", "sensors.qo");
req.begin_object("body");
    req.add("temp", temperature);   // runtime value
    req.add("humidity", read_rh()); // runtime value
req.end_object();
req.close();

char buf[256];
auto rsp = nc.transact(req.view(), buf);
```

See [JSON Buffer Builder](json-builder.md) for the full `JsonBuf` API,
including compile-time JSON construction.

This is the thinnest possible wrapper — it provides JSON envelope
validation, retry, and transport abstraction, and that is about it. You
would use it for protocol-level debugging, or when sending and parsing
requests in a highly constrained environment where even the lambda builder
overhead is too much.

## Choosing a Layer

| Layer | Complexity | Use when | You lose | AVR flash (vs typed) |
|-------|-----------|----------|----------|---------------------|
| **Typed API** (guided) | Methods and fields | Normal development | Nothing — this is the default | baseline (~24.7 KB) |
| **Typed API** (unguided) | Structs and fields | New request fields and values, cross-operation fields | Focused field surface | −210 B |
| **Lambda Request Builder** | Lambdas and strings | Unknown endpoints, migration from note-c | Most type safety | similar to typed |
| **Raw JSON + SAX sink** | Raw strings + custom `JsonSink` | Need streaming response parse (low response RAM) | Typed response fields | **−4.2 KB** |
| **Raw JSON + `JsonView` scan** | Raw strings + substring lookup | Known response shapes; flash is the bottleneck | Robust JSON parsing | **−13.8 KB** |

We recommend starting with the typed API and dropping down only when you
have a reason. Most firmware will never need anything beyond the guided
typed API. Full flash/RAM comparison table in the
[Arduino guide](platforms/arduino/guide.md#binary-size-comparison).
