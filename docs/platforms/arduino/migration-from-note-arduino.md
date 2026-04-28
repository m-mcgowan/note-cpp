# Migrating from note-arduino (note-c) to note-cpp

## Overview

If you're coming from the [note-arduino](https://github.com/blues/note-arduino)
library (which wraps [note-c](https://github.com/blues/note-c)), this guide
shows how your existing code maps to `note-cpp`. Each section shows the note-c
pattern on the left and the `note-cpp` equivalent on the right.

> All `note-cpp` code in this guide is taken from real examples compiled against note-cpp
> from [examples/arduino/migration/](../../../examples/arduino/migration/).
> The note-c examples are compiled from
> [tests/migration_notec.cpp](../../../tests/migration_notec.cpp).

Here's a few examples to illustrate the key differences in API style.

<table>
<tr><th>note-c</th><th>note-cpp</th></tr>
<tr>
<td>

```c
// Field names are strings, types are
// manual, no IDE help.
J *req = NoteNewRequest("hub.set");
JAddStringToObject(req, "product",
    "com.example.app");
JAddStringToObject(req, "mode", "periodic");
JAddNumberToObject(req, "outbound", 60);
NoteRequest(req);
```

</td>
<td>

```cpp
// Every field is a named member.
// IDE auto-completes after the dot.
nc.hub.set()
   .product("com.example.app")
   .mode("periodic")
   .outbound(60)
   .execute();

```

</td>
</tr>
<tr>
<td>

```c
// Body is a manual J* tree.
J *req = NoteNewRequest("note.add");
JAddStringToObject(req, "file", "sensors.qo");
J *body = JAddObjectToObject(req, "body");
JAddNumberToObject(body, "temp", 22.5);
JAddNumberToObject(body, "humidity", 60);
NoteRequest(req);
```
</td>
<td>

```cpp
// Body from a typed struct.
Readings r{.temperature = 22.5f, .humidity = 60};
nc.note.add()
   .file("sensors.qo")
   .body(r)
   .execute();

```
</td>
</tr>
<tr>
<td>

```c
// Stringly-typed — no compiler help
// if you misspell a field.
J *rsp = NoteRequestResponse(
    NoteNewRequest("card.version"));
char *ver = JGetString(rsp, "verison"); // typo!
char *dev = JGetString(rsp, "device");
NoteDeleteResponse(rsp);
```

</td>
<td>

```cpp
// Typed struct — misspelled fields
// won't compile.
auto r = nc.card.version().execute();
if (r) {
    auto ver = r.version; // typo would be an error
    auto dev = r.device;
}
```

</td>
</tr>
<tr>
<td>

```c
// Template type hints must match the
// body fields you'll send.
J *req = NoteNewRequest("note.template");
JAddStringToObject(req, "file", "sensors.qo");
J *body = JAddObjectToObject(req, "body");
JAddNumberToObject(body, "temperature",
    TFLOAT32);
JAddNumberToObject(body, "humidity", TINT16);
NoteRequest(req);
```

</td>
<td>

```cpp
// Same Readings struct auto-generates
// matching type hints — no duplication.
nc.note.templates().define("sensors.qo")
   .body(template_of(Readings()))
   .execute();




```

</td>
</tr>
</table>

The compiler catches what `note-c` defers to runtime: wrong field names, wrong types, wrong enum values, missing required fields. The rest of this guide covers each pattern in detail.




## Setup

<table>
<tr><th>note-arduino</th><th>note-cpp</th></tr>
<tr><td>

```c
#include <Notecard.h>

Notecard nc;

void setup() {
    nc.begin(Serial1, 9600);
}
```
</td><td>

```cpp
#include <note.hpp>

Notecard nc;

void setup() {
    nc.begin(Serial1, 9600);
}
```
</td></tr>
</table>

> On Arduino, `note.hpp` imports the API into the global namespace for
> developer convenience — `Notecard`, duration literals (`15_mins`, `5_s`),
> and other common names are available without qualification. See
> [namespace imports](../../feature-flags.md#namespace-imports) for how to
> customize this.

## Configuring the Hub (hub.set)

<table>
<tr><th>note-arduino</th><th>note-cpp</th></tr>
<tr><td>

<!-- snippet:tests/migration_notec.cpp:56-61 -->
```c
J *req = nc.newRequest("hub.set");
JAddStringToObject(req, "product",
    "com.example.app");
JAddStringToObject(req, "mode", "periodic");
JAddNumberToObject(req, "outbound", 60);
nc.sendRequest(req);
```

</td><td>

<!-- snippet:examples/arduino/migration/src/main.cpp:43-48 -->
```cpp
    nc.hub.set()
        .product("com.example.app")
        .mode("periodic")
        .outbound(60_mins)
        .execute();
}
```

</td></tr>
</table>

**Key differences from note-c:**
- No `J*` pointers to manage — no risk of leaking a request object.
- Field names are typed members. Misspell a field name and the compiler
  tells you — note-c compiles it silently.
- `outbound` is a bare integer in note-c — you have to remember it's in
  minutes. note-cpp accepts `60_mins` or `1_hours` with type-safe duration
  units that prevent accidental use, such as passing 30_seconds where a whole number of minutes are expected.
- On C++20, `"periodic"`, even as a string literal, is validated at compile time. A typo like
  `"perioidc"` is a compile error.

## API Styles

`note-cpp` offers 3 primary API styles:

1. fluent chain
2. direct assignment
3. designated initializers

### Fluent chain

Build and execute in one expression. Good when all fields are known upfront, or their values may be computed by a function.

The example above demonstrates the fluent chain approach.

### Direct assignment

Set fields individually. This is good when fields come from different sources or are set with inline conditional code.

<table>
<tr><th>note-arduino</th><th>note-cpp</th></tr>
<tr><td>

<!-- snippet:tests/migration_notec.cpp:67-79 -->
```c
J *req = nc.newRequest("hub.set");
JAddStringToObject(req, "product",
    "com.example.app");
JAddNumberToObject(req, "outbound", 60);
if (use_continuous) {
    JAddStringToObject(req, "mode",
        "continuous");
    JAddBoolToObject(req, "sync", true);
}
else {
    JAddStringToObject(req, "mode", "periodic");
}
nc.sendRequest(req);
```

</td><td>

```cpp
auto req = nc.hub.set();
req.product  = "com.example.app";
req.outbound = 60_mins;
if (use_continuous) {
    req.mode = "continuous";
    req.sync = true;
}
else {
    req.mode     = "periodic";
}
req.execute();


```

</td></tr>
</table>

### Designated initializers (C++20)

Brief and declarative. A good default choice when conditional behaviour
isn't needed.

<table>
<tr><th>note-arduino</th><th>note-cpp</th></tr>
<tr><td>

<!-- snippet:tests/migration_notec.cpp:56-61 -->
```c
J *req = nc.newRequest("hub.set");
JAddStringToObject(req, "product",
    "com.example.app");
JAddStringToObject(req, "mode", "periodic");
JAddNumberToObject(req, "outbound", 60);
nc.sendRequest(req);





```

</td><td>

<!-- snippet:examples/arduino/migration/src/main.cpp:54-64 -->
```cpp
bool use_continuous = false;
auto req = nc.hub.set();
req.product  = "com.example.app";
req.outbound = 60_mins;
if (use_continuous) {
    req.mode = "continuous";
    req.sync = true;
} else {
    req.mode = "periodic";
}
req.execute();
```

</td></tr>
</table>

> **Note:** Designated initializers require the fields in alphabetical order. This is specific to
> the designated initializer syntax — fluent chains and direct assignment
> accept fields in any order.

### Why three ways?

They're different tools for different shapes of code. Fluent chains are concise when configuring and executing in
one statement. Direct assignment is natural when fields come from variables
or conditional logic. Designated initializers read like data, not procedure
— brief and clear when the values are known at the call site.

## Sending sensor data (note.add)

**note-arduino:**

<!-- snippet:tests/migration_notec.cpp:84-95 -->
```c
struct Readings {
    float temperature;
    int16_t humidity;
};

Readings r{.temperature = 22.5f, .humidity = 60};
J *req = nc.newRequest("note.add");
JAddStringToObject(req, "file", "sensors.qo");
J *body = JAddObjectToObject(req, "body");
JAddNumberToObject(body, "temp", r.temperature);
JAddNumberToObject(body, "humidity", r.humidity);
nc.sendRequest(req);
```

**note-cpp:**

<!-- snippet:examples/arduino/migration/src/main.cpp:24-28 -->
```cpp
struct Readings {
    float temperature;
    int16_t humidity;
    NOTE_FIELDS(temperature, humidity)  // not needed on C++20
};
```

<!-- snippet:examples/arduino/migration/src/main.cpp:71-76 -->
```cpp
    Readings r{.temperature = 22.5f, .humidity = 60};
    nc.note.add()
        .file("sensors.qo")
        .body(r)
        .execute();
}
```

**With error handling:**

In note-c, you check for a null response, then check the `err` field — an
unstructured string you have to parse yourself:

<!-- snippet:tests/migration_notec.cpp:101-110 -->
```c
J *rsp = nc.requestAndResponse(req);
if (rsp == NULL) {
    Serial.println("no response");
} else if (nc.responseError(rsp)) {
    // "note.add: queue full" — you parse this yourself
    Serial.println(JGetString(rsp, "err"));
    nc.deleteResponse(rsp);
} else {
    nc.deleteResponse(rsp);
}
```

In note-cpp, the result carries structured error information. The error code
tells you where the failure happened (e.g. notecard, json parsing, transport) and the cause tells you why.

```cpp
auto result = nc.note.add()
    .file("sensors.qo")
    .body(r)
    .execute();
if (!result) {
    Serial.println(result.error());
}
```

`note-cpp` on Arduino provides some time-savers, sch as converting most objects to a Printable so they are easily used with arduino streams.

Example outputs:

 - notecard error: `notecard: note.add: queue full`
 - transport failure: `send_failed[timeout]: no response within deadline`


You can save the error to a variable when you want to inspect it in more detail:

```
auto err = result.error();
```

Error details:
- `err.code` tells you which layer failed:
  - `Error::SendFailed` — never reached the Notecard (safe to retry)
  - `Error::Notecard` — Notecard returned an error, available as `err.message`
  - `Error::Json` — response couldn't be parsed
- `err.cause` tells you why:
  - `Cause::Timeout`, `Cause::HalError`, `Cause::CrcMismatch`, etc.
- `err.message` is the Notecard's error string:
  - `"note.add: file not found"`
  - `"note.add: queue full"`

See [Error Handling](../../error-handling.md) for the full reference including
retry safety levels.

**Key differences:**
- Both sides define the same struct, note-cpp uses it directly as the
  body without manual field-by-field JSON construction.
- No two-level pointer management (`req` then `body`). If you forget
  `JAddObjectToObject` in note-c, the fields end up on the request itself
  and are silently ignored.

For quick schemaless bodies without defining a struct, `json_fmt` (C++20)
is the most concise option:

```cpp
nc.note.add()
    .file("sensors.qo")
    .body(json_fmt<R"({"temp":{},"humidity":{}})">(temp, humidity))
    .execute();
```

The JSON structure is validated at compile time — malformed JSON or wrong
argument count/types are compile errors. At runtime it's just string
concatenation, no heap allocation. See [Body Values](../../body-values.md)
for all approaches.


## Registering templates

<table>
<tr><th>note-arduino</th><th>note-cpp</th></tr>
<tr><td>

<!-- snippet:tests/migration_notec.cpp:115-122 -->
```c
// Type constants from note.h — you pick the
// right one for each field manually.
J *req = nc.newRequest("note.template");
JAddStringToObject(req, "file", "sensors.qo");
J *body = JAddObjectToObject(req, "body");
JAddNumberToObject(body, "temp", TFLOAT32);
JAddNumberToObject(body, "humidity", TINT16);
nc.sendRequest(req);
```

</td><td>

<!-- snippet:examples/arduino/migration/src/main.cpp:96-98 -->
```cpp
nc.note.templates().define("sensors.qo")
    .body(note::template_of(Readings()))
    .execute();





```

</td></tr>
</table>

**Key differences:**
- note-c provides named constants (`TFLOAT32`, `TINT16`, etc.) but you still
  have to pick the right one for each field and keep them in sync with your
  struct. note-cpp derives the type hints from the struct's C++ types
  automatically — change a field from `float` to `double` and the template
  updates itself.
- One struct for everything. Define `Readings` once, and the library uses it
  for sending, receiving, and template registration.

If you prefer, you can also use an explicit template parameter instead of an instance:
```cpp
   .body(template_of<Readings>())
```

## Reading temperature (card.temp)

<table>
<tr><th>note-arduino</th><th>note-cpp</th></tr>
<tr><td>

<!-- snippet:tests/migration_notec.cpp:127-143 -->
```c
J *rsp = nc.requestAndResponse(
    nc.newRequest("card.temp"));
if (rsp == NULL) {
    Serial.println("no response");
} else if (nc.responseError(rsp)) {
    Serial.println(JGetString(rsp, "err"));
    nc.deleteResponse(rsp);
} else {
    double temp = JGetNumber(rsp, "value");
    Serial.println(temp);
    nc.deleteResponse(rsp);
}

// Configure periodic monitoring
J *req = nc.newRequest("card.temp");
JAddNumberToObject(req, "minutes", 5);
nc.sendRequest(req);
```

</td><td>

<!-- snippet:examples/arduino/migration/src/main.cpp:105-111 -->
```cpp
    auto r = nc.card.temp().read().execute();
    if (r) {
        Serial.println(r.value);
    } else {
        Serial.println(r.error());
    }
}










```

</td></tr>
</table>

**Key differences:**
- Avoided manual response lifecycle (`deleteResponse`). The response is cleaned up automatically when it goes out of scope ([RAII](https://en.cppreference.com/w/cpp/language/raii.html)). Numeric and boolean fields are plain values — safe to keep indefinitely. String fields (`string_view`) are valid until the next request unless you use an arena. See [Response Lifetimes](../../response-lifetimes.md).
- `JGetNumber(rsp, "value")` returns 0.0 on misspelling with no error.
  `r.value` is a named member — misspelling won't compile.
- `card.temp` can do several things - it can read the current
  temperature or configure periodic monitoring depending on which fields you
  send. note-cpp has `.read()` and `.configure()` — each with only the fields that apply so the intent is clear.

## Reading device info (card.version)

<table>
<tr><th>note-arduino</th><th>note-cpp</th></tr>
<tr><td>

<!-- snippet:tests/migration_notec.cpp:143-150 -->
```c
nc.sendRequest(req);
}

// ── card.version ────────────────────────────────────────────────────────
void card_version() {
J *rsp = nc.requestAndResponse(
    nc.newRequest("card.version"));
if (rsp != NULL) {
```

</td><td>

```cpp
auto r = nc.card.version().execute();
if (r) {
    // Print individual fields
    Serial.print("ver=");
    Serial.println(r.version);
    // Or print the entire response as JSON
    Serial.println(r);
}
```

</td></tr>
</table>

**Key differences:**
- `JGetString` returns a `char*` that you must not use after `deleteResponse`.
  note-cpp returns `string_view` with the same lifetime constraint, but you
  never have to think about `deleteResponse` — it happens automatically.
- `r.version` and `r.device` are typed members, not string lookups.
- Response fields and full responses are Arduino `Printable` —
  `Serial.print(r.version)` just works. Avoid `printf("%.*s")` with
  `string_view` — use `Serial.print()` instead.
  See the [Arduino Guide](guide.md) for printing patterns, String
  conversion, and AVR setup.

## ATTN pin — arming for interrupts

<table>
<tr><th>note-arduino</th><th>note-cpp</th></tr>
<tr><td>

<!-- snippet:tests/migration_notec.cpp:155-166 -->
```c
    nc.deleteResponse(rsp);
}
}

// ── card.attn ───────────────────────────────────────────────────────────
void card_attn() {
// Arm for connectivity + motion triggers
J *req = nc.newRequest("card.attn");
JAddStringToObject(req, "mode",
    "arm,connected,motion");
JAddNumberToObject(req, "seconds", 120);
nc.sendRequest(req);
```

</td><td>

```cpp
// Arm for specific triggers — the "arm,"
// prefix and mode string are built for you.
nc.card.attn().arm()
    .connected()
    .motion()
    .seconds(120_s)
    .execute();

// Disarm all triggers
nc.card.attn().disarm().execute();


```

</td></tr>
</table>

Additional ATTN operations:

```cpp
// Re-arm (idempotent — safe to call every time ATTN fires)
nc.card.attn().rearm()
    .connected().motion().seconds(120_s)
    .execute();

// Disable/enable ATTN processing entirely
nc.card.attn().off().execute();
nc.card.attn().on().execute();

// Raw Request for full control (string validated at compile time)
note::api::CardAttn::Request req;
req.mode = "arm,connected,motion";
req.seconds = 120;
nc.execute(req);
```

**Key differences:**
- No manual string concatenation for mode flags. In note-c, you build
  `"arm,connected,motion"` yourself — get the commas or names wrong and it
  fails silently. note-cpp has named methods (`.connected()`, `.motion()`)
  and flag constants (`note::attn::connected | note::attn::motion`).
- Intent-based types (`Arm`, `Rearm`, `Disarm`, `Sleep`, `Watchdog`, `Off`, `On`)
  expose only the fields relevant to that operation.
- The base `Request` type accepts all mode values if you need full control —
  string literals are validated at compile time (C++20).

**Querying ATTN state and iterating array fields:**

<table>
<tr><th>note-c</th><th>note-cpp</th></tr>
<tr><td>

```c
J *req = NoteNewRequest("card.attn");
JAddBoolToObject(req, "verify", true);
J *rsp = NoteRequestResponse(req);
J *files = JGetObject(rsp, "files");
J *file;
JArrayForEach(file, files) {
    Serial.printf("  %s\n",
        file->valuestring);
}
NoteDeleteResponse(rsp);
```

</td><td>

```cpp
// Array elements are null-terminated
// string_views — use directly as
// const char* or string_view.
auto r = nc.card.attn().query()
    .execute();
if (r) {
    for (auto& f : r.files) {
        Serial.println(f);
    }
}
```

</td></tr>
</table>

## ATTN pin — sleep with state

<table>
<tr><th>note-arduino</th><th>note-cpp</th></tr>
<tr><td>

```c
// Sleep — save state across reset
J *req = nc.newCommand("card.attn");
JAddStringToObject(req, "mode", "sleep");
JAddNumberToObject(req, "seconds", 3600);
JAddStringToObject(req, "payload", "checkpoint-v1");
nc.sendRequest(req);
// Enter deep sleep...

// On wake — retrieve saved state
J *req = nc.newRequest("card.attn");
JAddBoolToObject(req, "start", true);
J *rsp = nc.requestAndResponse(req);
if (rsp != NULL) {
    double time = JGetNumber(rsp, "time");
    if (time != 0) {
        char *payload = JGetString(rsp, "payload");
        // payload is "checkpoint-v1"
    }
    nc.deleteResponse(rsp);
}
```

</td><td>

```cpp
// Sleep — save state across reset
auto req = nc.card.attn().sleep();
req.seconds = 1_hours;
req.payload = "checkpoint-v1";
req.execute();
// Enter deep sleep...

// On wake — retrieve saved state
auto r = nc.card.attn().retrieve()
    .execute();
if (r && r.time != 0) {
    // r.payload is "checkpoint-v1"
}







```

</td></tr>
</table>

**Key differences:**
- Sleep and retrieve are distinct types — the compiler ensures you don't
  mix their fields.
- `1_hours` instead of `3600` — clear intent, impossible to confuse with
  minutes.
- `r.payload` and `r.time` are typed members. No `JGetString` / null checks.

## Environment variables (env.get)

`env.get` has four modes — single variable, multiple named, all,
change-polling — and note-cpp supports all four with typed responses.
The example below covers the single-variable case. For the full story
(multi-variable reads, streaming directly into a user struct, and
`env.modified` change detection) see
[**`docs/environment-variables.md`**](../../environment-variables.md)
and the runnable example
[`examples/stdcpp/env-vars.cpp`](../../../examples/stdcpp/env-vars.cpp).
For conceptual background (hierarchy, reserved system variables) see
Blues' [Understanding Environment Variables](https://dev.blues.io/guides-and-tutorials/notecard-guides/understanding-environment-variables/).

<table>
<tr><th>note-arduino</th><th>note-cpp</th></tr>
<tr><td>

```c
// Read a single env var
J *req = nc.newRequest("env.get");
JAddStringToObject(req, "name", "interval");
J *rsp = nc.requestAndResponse(req);
if (rsp != NULL) {
    char *text = JGetString(rsp, "text");
    int interval = atoi(text);
    nc.deleteResponse(rsp);
}

// Set a default
J *req = nc.newRequest("env.default");
JAddStringToObject(req, "name", "interval");
JAddStringToObject(req, "text", "60");
nc.sendRequest(req);
```

</td><td>

```cpp
// Read a single env var
auto r = nc.env.get()
    .name("interval")
    .execute();
if (r) {
    auto text = r.text;  // "60"
}

// Set a default
nc.env.setDefault("interval", "60")
    .execute();




```


</td></tr>
</table>

## Polymorphic APIs

Some Notecard endpoints behave differently depending on which fields you
send. In note-c, you use the same function and hope you set the right
combination. In note-cpp, each behavior is a distinct method.

The Notecard documentation describes these endpoints with their standard
request names — those names still apply. note-cpp just makes the different
behaviors explicit:

### note.get — read vs pop

<table>
<tr><th>note-arduino</th><th>note-cpp</th></tr>
<tr><td>

```c
// Read by ID (non-destructive)
J *req = nc.newRequest("note.get");
JAddStringToObject(req, "file", "data.db");
JAddStringToObject(req, "note", "my-id");
J *rsp = nc.requestAndResponse(req);

// Pop from queue (destructive)
J *req = nc.newRequest("note.get");
JAddStringToObject(req, "file", "data.qi");
JAddBoolToObject(req, "delete", true);
J *rsp = nc.requestAndResponse(req);
```

</td><td>

```cpp
// Read by ID — note.get without delete
auto r = nc.note.read("data.db")
    .noteId("my-id")
    .execute();

// Pop from queue — note.get with delete
auto r = nc.note.pop("data.qi")
    .execute();



```

</td></tr>
</table>

Both produce `note.get` on the wire. The difference: `read()` can't
accidentally include `delete:true`, and `pop()` always includes it.
Each variant only exposes the fields that apply.
See [Polymorphic APIs](../../intent-scoped-apis.md) for the full list.

### card.temp — read vs configure

<table>
<tr><th>note-arduino</th><th>note-cpp</th></tr>
<tr><td>

```c
// Read current temperature
J *req = nc.newRequest("card.temp");
J *rsp = nc.requestAndResponse(req);
double temp = JGetNumber(rsp, "value");

// Configure periodic monitoring
J *req = nc.newRequest("card.temp");
JAddNumberToObject(req, "minutes", 5);
nc.sendRequest(req);
```

</td><td>

```cpp
// Read current temperature
auto r = nc.card.temp().read().execute();
double temp = r.value;

// Configure periodic monitoring
nc.card.temp().configure()
    .minutes(5)
    .execute();

```

</td></tr>
</table>

### Non-polymorphic endpoints

Most endpoints have a single behavior — these work exactly as the
Notecard documentation describes. The request name maps directly to a
method:

```cpp
// Wire name shown in comment:
nc.card.version().execute();     // card.version
nc.card.status().execute();      // card.status
nc.hub.sync().execute();         // hub.sync
nc.hub.status().execute();       // hub.status
```

## Convenience shortcuts

Frequently-used operations have shorthand methods that pre-fill required
parameters:

<table>
<tr><th>note-arduino</th><th>note-cpp</th></tr>
<tr><td>

```c
// Read a note by ID
J *req = nc.newRequest("note.get");
JAddStringToObject(req, "file", "data.db");
JAddStringToObject(req, "note", "my-id");

// Pop from queue
J *req = nc.newRequest("note.get");
JAddStringToObject(req, "file", "data.qi");
JAddBoolToObject(req, "delete", true);

// Set an env default
J *req = nc.newRequest("env.default");
JAddStringToObject(req, "name", "interval");
JAddStringToObject(req, "text", "60");
```

</td><td>

```cpp
// Read a note by ID
nc.note.read("data.db")
    .noteId("my-id");

// Pop from queue
nc.note.pop("data.qi");

// Set an env default
nc.env.setDefault("interval", "60");





```

</td></tr>
</table>

The shorthand methods accept required parameters directly — no need to
set them separately.

## Receiving data (into)

When reading notes with structured data, note-c returns raw JSON that
you parse field by field. note-cpp parses the body directly into your
struct during the streaming SAX pass — no intermediate buffer:

<table>
<tr><th>note-arduino</th><th>note-cpp</th></tr>
<tr><td>

```c
J *rsp = nc.requestAndResponse(
    nc.newRequest("note.get"));
if (rsp && !nc.responseError(rsp)) {
    J *body = JGetObject(rsp, "body");
    float temp = JGetNumber(body, "temp");
    int hum = JGetInt(body, "humidity");
    nc.deleteResponse(rsp);
}
```

</td><td>

```cpp
Readings data;
auto r = nc.note.read("data.qi")
    .into(data)
    .execute();
if (r) {
    // data.temperature, data.humidity
    // populated during SAX parse
}
```

</td></tr>
</table>

The same `Readings` struct used for sending and template registration
also works for receiving. See [Body Values](../../body-values.md) for details.

## Type-safe units

Duration fields use distinct types that allow time units to be expressed in larger units,
while also preventing accidental mix-ups - a value in the wrong unit is a compile error, not a silent bug:

```cpp
using namespace note::literals;

nc.hub.set()
    .outbound(60_mins)           // Minutes — matches the wire format
    .execute();

nc.hub.set()
    .outbound(2_hours)           // Hours → Minutes (120 on the wire)
    .execute();

nc.card.sleep()
    .seconds(12_hours)           // Hours → Seconds (43200 on the wire)
    .execute();

// nc.hub.set().outbound(60_s);  // Compile error: Seconds ≠ Minutes
```

In note-c, `outbound` is a plain integer — you have to know from the
docs that it's in minutes - sometimes the requests make the unit clear by the name
(e.g. "seconds") but sometimes not. The duration type system avoids any ambiguity. 
See [Duration Units](../../duration-units.md) for the full type system.

## Named constants

Fields with a fixed set of valid values provide named constants for
discoverability. You can still use strings — the constants are there
for IDE autocomplete and to avoid typos:

```cpp
using mode = note::api::HubSet::mode_t;

nc.hub.set().mode("periodic").execute();        // string — works fine
nc.hub.set().mode(mode::periodic).execute();    // constant — autocomplete-friendly
```

On C++20, string literals are validated at compile time — a typo like
`"perioidc"` is a compile error regardless of which form you use.

## Nested endpoints

Multi-segment endpoint names map to nested accessors:

```cpp
nc.card.binary.status();         // card.binary (status)
nc.card.binary.put();            // card.binary.put
nc.card.binary.get();            // card.binary.get

nc.card.location();              // card.location
nc.card.location.mode.periodic(); // card.location.mode (periodic variant)
nc.card.location.track();        // card.location.track

nc.hub.sync();                   // hub.sync
nc.hub.sync.status();            // hub.sync.status
```

The dot-chain mirrors the Notecard API naming, so the documentation
maps directly to code.

## Error handling

<table>
<tr><th>note-arduino</th><th>note-cpp</th></tr>
<tr><td>

<!-- snippet:tests/migration_notec.cpp:214-225 -->
```c
JAddStringToObject(req2, "text", "60");
nc.sendRequest(req2);
}

// ── error handling ──────────────────────────────────────────────────────
void error_handling() {
J *rsp = nc.requestAndResponse(
    nc.newRequest("card.version"));
if (rsp == NULL) {
    Serial.println("no response");
} else if (nc.responseError(rsp)) {
    char *err = JGetString(rsp, "err");
```

</td><td>

```cpp
auto r = nc.card.version().execute();
if (!r) {
    auto err = r.error();
    // err.code:    Error::Notecard, SendFailed, etc.
    // err.cause:   Cause::Timeout, HalError, etc.
    // err.message: human-readable string
    Serial.println(err);
} else {
    // use r.version, r.device, etc.
}


```

</td></tr>
</table>

**Key differences:**
- Structured errors with code, cause, and message — not a bare string.
  `Error::SendFailed` tells you the request never left the host;
  `Error::Notecard` means the device returned an error.
- No null pointer checks. The result type is truthy on success, falsy
  on failure — one path, not three.
- No `deleteResponse` — cleanup is automatic.

### Retry safety

Every request type carries a compile-time safety classification that tells
you whether retrying a failed request is safe:

| Safety | Meaning | Retry? |
|--------|---------|--------|
| `ReadOnly` | No side effects (e.g. `card.version`) | Always safe |
| `Idempotent` | Same result if repeated (e.g. `hub.set`) | Always safe |
| `NonIdempotent` | May have different effect if repeated (e.g. `note.add`) | Only if `Error::SendFailed` |
| `Destructive` | Consumes or deletes data (e.g. `note.get` delete) | Only if `Error::SendFailed` |

```cpp
auto result = nc.note.add().file("sensors.qo").body(r).execute();
if (!result) {
    // SendFailed means the request never reached the Notecard — safe to retry.
    // ResponseLost means it may have been processed — check the safety level.
    if (result.error().code == Error::SendFailed
        || is_safe_to_retry(NoteAdd::safety)) {
        // retry...
    }
}

// Or check at compile time:
static_assert(CardVersion::safety == Safety::ReadOnly);
static_assert(is_safe_to_retry(HubSet::safety));
static_assert(!is_safe_to_retry(NoteAdd::safety));
```

In note-c, there's no equivalent — you have to know from the docs which
requests are safe to retry, and currently note-c retries all requests.

## Fire-and-forget commands

<table>
<tr><th>note-arduino</th><th>note-cpp</th></tr>
<tr><td>

<!-- snippet:tests/migration_notec.cpp:239-240 -->
```c
nc.sendRequest(req);
}
```

</td><td>

<!-- snippet:examples/arduino/migration/src/main.cpp:224-224 -->
```cpp
nc.hub.sync().command();

```

</td></tr>
</table>

## Raw JSON passthrough

For serial passthrough protocols, debug consoles, or any situation where you
need to send pre-formatted JSON to the Notecard:

<table>
<tr><th>note-c</th><th>note-cpp</th></tr>
<tr><td>

```c
// Parse, send, get response
J *req = JParse(json_string);
J *rsp = notecard.requestAndResponse(req);
char *s = JPrintUnformatted(rsp);
// use s...
JFree(s);
JDelete(rsp);



```

</td><td>

```cpp
// BareNotecard — standalone raw JSON transport
note::StreamingTransport transport(hal);
note::BareNotecard bare(transport);

char buf[512];
auto rsp = bare.transact(json_string, buf);
if (rsp) { /* *rsp is the response */ }

// Fire-and-forget
bare.send(json_command);
```

</td></tr>
</table>

`BareNotecard` validates the JSON (SAX-parsed) before sending, then
transmits the raw bytes through the transport and reads the response
into a caller-provided buffer. No allocator, no JSON backend, no typed
API — just validated JSON in/out. Equivalent to note-c's
`requestAndResponse(JParse(json))`.

`transact()` and `send()` are also available on `Notecard` directly
for firmware that uses both the typed API and raw passthrough:

```cpp
// Typed API for firmware operations
nc.hub.set().product("com.example").execute();

// Raw passthrough for external commands
char buf[512];
auto rsp = nc.transact(R"({"req":"card.version"})", buf);
```

## Binary data transfers (card.binary)

Both libraries provide high-level convenience functions and low-level
typed requests for binary data transfers.

### High-level

<table>
<tr><th>note-arduino</th><th>note-cpp</th></tr>
<tr><td>

```c
uint8_t buf[1024];
// ... fill buf with data ...

NoteBinaryStoreReset();
NoteBinaryStoreTransmit(buf, data_len,
    sizeof(buf), 0);
NoteBinaryStoreReceive(buf,
    sizeof(buf), 0, data_len);
```

</td><td>

```cpp
uint8_t buf[1024];
// ... fill buf with data ...

nc.binaryReset();
nc.binaryStore(buf, data_len);

nc.binaryReceive(buf, sizeof(buf));

```

</td></tr>
</table>

### Low-level

<table>
<tr><th>note-arduino</th><th>note-cpp</th></tr>
<tr><td>

```c
uint8_t buf[1024];

J *req = nc.newRequest("card.binary");
JAddBoolToObject(req, "delete", true);
nc.sendRequest(req);

req = nc.newRequest("card.binary.put");
JAddNumberToObject(req, "cobs",
    cobs_len);
JAddStringToObject(req, "status",
    md5_hex);
nc.sendRequest(req);
// ... send raw COBS bytes ...
```

</td><td>

```cpp
uint8_t buf[1024];

nc.card.binary.clear().execute();
nc.card.binary.put()
    .data(buf, data_len)
    .execute();







```

</td></tr>
</table>

**Key differences:**
- note-c's `NoteBinaryStoreTransmit` encodes COBS in-place, mutating
  your buffer. note-cpp stream-encodes from a const source — your data
  is untouched.
- note-c requires a buffer large enough for the *encoded* data.
  note-cpp works with raw-size buffers only.

## Firmware version and SKU safety

note-c has no awareness of which Notecard firmware version you're targeting
or which hardware variant you're running on. If you use a field that was
added in firmware 6.2.3 on a device running 5.0, the Notecard silently
ignores it. If you call `card.wifi` on a cellular Notecard, you get a
runtime error.

note-cpp catches both at compile time:

**Firmware version gating** — fields added in newer firmware versions are
marked with `[[deprecated]]` when you target an older version. Define
`NOTE_API_VERSION` to your minimum supported firmware, and the compiler
warns you about fields that won't work:

```cpp
#define NOTE_API_VERSION NOTE_VERSION(5, 0, 0)
#include <note/api.hpp>

nc.hub.set()
    .product("com.example.app")
    .details("...")     // warning: requires firmware >= 6.2.3
    .execute();
```

With `NOTE_API_STRICT` defined, the warning becomes a compile error.

**Hardware targeting** (C++20) — constrain your `Api` to a specific Notecard
variant. Endpoints that don't support that hardware produce warnings (or
errors in strict mode):

```cpp
Api<Hardware::WiFi> nc(notecard);
nc.card.wifi();   // OK — WiFi endpoint on WiFi hardware
nc.card.sleep();  // OK — universal endpoint
// nc.card.lora(); // warning: not available on WiFi
```

Neither of these has an equivalent in note-c — there, incompatible requests
compile silently and fail at runtime on the device.

## What to expect

### What you gain from C++

Moving from C to C++ brings benefits independent of note-cpp:

- **Automatic cleanup** — no manual `deleteResponse` / `JDelete`.
  Responses clean up when they go out of scope. No leak risk.
  You can have responses outlive the current scope —
  see [Response Lifetimes](../../response-lifetimes.md).
- **Type safety** — field types are checked at compile time. No more
  `JGetNumber` returning 0.0 on a misspelled field name.
- **Namespaces** — no global symbol pollution. Your code and the
  Notecard API don't compete for names.
- **Zero-copy responses** — response fields are views into the
  transport buffer, not heap-allocated copies.
- **Compile-time validation** (C++20) — enum values, flag combinations,
  and JSON body structure are checked by the compiler before the code
  reaches the device.

### Binary size

**AVR (ATmega328P, 32 KB flash / 2 KB RAM)** — using `StaticNotecard`
with streaming transport (see `tools/binary-size-comparison/`):

| | note-c | note-cpp | Delta |
|---|---|---|---|
| Flash | 24,646 (76%) | 14,592 (45%) | **-10,054 (-41%)** |
| RAM | 739 (36%) | 712 (35%) | **-27 (-4%)** |

note-cpp is 41% smaller on flash because the streaming transport and
SAX parser eliminate the cJSON tree, and `StaticNotecard` has zero
virtual dispatch overhead.

**ESP32-S3** — with the same cJSON backend and identical operations
(hub.set, note.template, card.temp, note.add):

| | note-c | note-cpp | Delta |
|---|---|---|---|
| Code (.flash.text) | 198,540 | 196,596 | **-1,944 (-1.0%)** |
| Constants (.flash.rodata) | 97,260 | 91,464 | **-5,796 (-6.0%)** |

note-cpp is slightly smaller because `dtoa` (409 bytes) replaces
note-c's `JNtoA` (1,597 bytes), and `string_view` avoids string
duplication.

### Memory allocation

| Concern | note-c | note-cpp |
|---|---|---|
| **Heap per request** | cJSON `malloc`/`free` per request | `BufferJsonBackend` — stack-allocated, zero heap in steady state |
| **Response lifetime** | Caller must `deleteResponse` | RAII — automatic cleanup, `string_view` into transport buffer |
| **Transport buffers** | `malloc`'d, freed per call | Reused `std::string` member — no allocation after warmup |
| **Error strings** | `ERRSTR(long, short)` macro | `string_view` literals — linker deduplicates |

With the default `BufferJsonBackend`, note-cpp uses ~1.7 KB more static
RAM (the stack-allocated JSON build buffer + token array) but performs
zero heap allocations in steady state. This tradeoff avoids heap
fragmentation — a common problem on long-running embedded devices.

For a direct comparison using the same heap-allocated backend (cJSON),
both libraries have similar per-request heap usage.

### What note-cpp doesn't support (yet)

- **ESP8266** — `tl::expected` polyfill incompatible with GCC 10.3
- **Apple Clang consteval** — string literal validation disabled due to
  a compiler bug (named constants and flag methods always work)

### AVR support

note-cpp runs on AVR (ATmega328P) with the streaming transport path.
Uses `StaticNotecard` for zero-vtable dispatch and `avr-libstdcpp` for
standard library headers. See `tools/binary-size-comparison/` for
the full PlatformIO configuration. Key build flags:

- `NOTE_NO_STD_STRING` — excludes `std::string`/`std::functional` paths
- `NOTE_NO_MD5`, `NOTE_NO_CRC` — excludes lookup tables
- `NOTE_EXTRAS=0` — disables dynamic fields (saves ~168 bytes per request)
- `NOTE_SHORT_ERRORS=1` — collapses error messages to save flash

### Controlling binary size

Features are controlled structurally (template parameters, linker
`--gc-sections`) rather than a single preprocessor flag. For the
lowest memory path, `sax_parse_streaming()` parses responses
incrementally with only a small scratch buffer (`SaxStreamBuf`,
default 384 bytes on the stack).

See [Known Issues](../../known-issues.md) for details on the Clang limitation.

## Gradual migration

You don't have to port everything at once. note-cpp's buffered path
supports a `request()` method that mirrors note-c's `J*` workflow,
letting you migrate one request at a time:

1. **Replace the library and setup.** Swap `note-arduino` for `note-cpp`
   in your dependencies. Replace the `Notecard` constructor and `begin()`
   call — see the setup section above. This is the only step that must
   happen all at once (both libraries can't share the same transport).

2. **Keep existing request patterns temporarily.** Use `nc.request()` with
   lambda builders for requests you haven't ported yet:

   ```cpp
   // Before (note-c):
   J *req = NoteNewRequest("hub.set");
   JAddStringToObject(req, "product", "com.example.app");
   NoteRequest(req);

   // After (note-cpp, same pattern):
   nc.request("hub.set", [](note::JsonBuilder& b) {
       b.add("product", "com.example.app");
   });
   ```

3. **Migrate individual requests to the typed API** at your own pace:

   ```cpp
   // Final form:
   nc.hub.set().product("com.example.app").execute();
   ```

Both styles coexist in the same project — typed API and `request()`
lambdas use the same underlying transport. Migrate the easy requests
first (hub.set, card.version), then tackle complex ones (binary
transfers, body structs) when you're comfortable.

### Running note-cpp alongside note-c

For large projects where you can't swap the library all at once, you can
run note-cpp on top of note-c's existing transport. Implement
`IBufferedTransport` and delegate each request to
`NoteRequestResponseJSON()`:

<!-- snippet:bridge-extern examples/stdcpp/note-c-bridge.cpp:23-23 -->
```cpp
extern "C" char* NoteRequestResponseJSON(const char* reqJSON);
```

<!-- snippet:bridge-transport examples/stdcpp/note-c-bridge.cpp:34-70 -->
```cpp
/// Delegates every request to note-c's NoteRequestResponseJSON so note-c
/// owns the serial/I2C bus and note-cpp sits on top with its typed API.
class NoteCTransport : public note::IBufferedTransport {
    std::string rsp_buf_;
public:
    note::Result<note::string_view> transact(note::string_view req, uint32_t) override {
        std::string req_str(req.data(), req.size());
        char* rsp = NoteRequestResponseJSON(req_str.c_str());
        if (rsp == nullptr) {
            return note::make_error(note::Error::ResponseLost, "no response");
        }
        rsp_buf_ = rsp;
        std::free(rsp);
        return note::string_view(rsp_buf_);
    }
    note::Result<void> send(note::string_view req) override {
        auto r = transact(req, 0);
        if (!r) return note::Unexpected(r.error());
        return {};
    }
    void reset() override {}
    void abort() override {}

    // Minimal Hal stub — note-c owns the actual hardware, so the bridge's
    // Hal is purely a placeholder so the inherited Notecard timing path
    // has something valid to call. Returning 0/no-op is safe because all
    // wire bytes go through NoteRequestResponseJSON above.
    struct NoopHal : note::Hal {
        bool transmit(const uint8_t*, size_t) override { return true; }
        note::Result<size_t> read(uint8_t*, size_t, uint32_t) override { return note::Result<size_t>{size_t{0}}; }
        bool reset() override { return true; }
        bool write_line_terminator() override { return true; }
        uint32_t millis() override { return 0; }
        void delay(uint32_t) override {}
    } hal_;
    note::Hal& hal() override { return hal_; }
};
```

Wire it into a `Notecard` + `Api`:

<!-- snippet:bridge-wiring examples/stdcpp/note-c-bridge.cpp:74-83 -->
```cpp
int main() {
    MockBackend backend;
    NoteCTransport transport;
    note::Notecard nc(backend, transport);
    note::Api api(nc);

    // Typed API calls route through note-c's existing transport.
    api.hub.set().product("com.example.app").mode("periodic").execute();
    return 0;
}
```

Both libraries share a single Notecard connection — no hardware
conflicts. Existing `NoteNewRequest` / `J*` code continues to work
unchanged. The complete working example is at
[`examples/stdcpp/note-c-bridge.cpp`](examples/stdcpp/note-c-bridge.cpp).

## Migration checklist

1. **Replace `Notecard` with `note::arduino::Notecard`.** One include, same `begin()`
   call — see the setup section above.

2. **Replace `J*` request building with typed API calls.** Every
   `nc.newRequest("...")` + `JAdd*` sequence becomes `api.xxx.yyy()` +
   fluent setters or direct assignment.

3. **Replace `JGet*` response reading with typed members.** Every
   `JGetString(rsp, "field")` becomes `r.field`.

4. **Remove manual memory management.** Delete all `deleteResponse`,
   `JDelete`, null checks on `J*`. Responses are RAII values.

5. **Replace manual template registration.** Replace per-field `TFLOAT32`/`TINT16`
   constants with `note::template_of<YourStruct>()` — type hints derived
   automatically from C++ types.

6. **Replace string constants with type-safe alternatives.**
   - `"periodic"` → `mode_t::periodic` (named constant) or validated literal
   - `60` (minutes) → `60_mins` or `1_hours` (duration types)
   - `"arm,connected"` → `.arm().connected()` (named flag methods)

## Common migration pitfalls

### Printing response values

`ResponseField<string_view>` implements Arduino `Printable`, so
`Serial.println(rsp.version)` works directly. For other types —
array elements, full responses, request fields — use the `printable()`
wrapper:

```cpp
// ResponseField — Printable, works directly
Serial.println(rsp.version);

// Array elements — use c_str() or printable()
for (auto& f : result.files) {
    Serial.println(f.c_str());     // null-terminated — works directly
    Serial.println(printable(f));  // via printable() wrapper (alternative)
}

// Full response — use printable()
Serial.println(printable(result));
```

### `noteId` vs `note` field name

Several Notecard requests use a JSON field named `"note"` for the note ID.
In `note-cpp`, the C++ accessor is `noteId` (because `note` is the library's
namespace). Three ways to set it:

```cpp
// Member form (typed):
auto req = nc.note.get();
req.noteId = "my-id";

// Factory form (typed):
nc.note.get().noteId("my-id").execute();

// Wire-name form (subscript, matches the JSON you'd write):
auto req = nc.note.get();
req["note"] = "my-id";   // maps to noteId internally
```

The subscript form is convenient when you're porting code that already uses
the wire name (`JAddStringToObject(req, "note", ...)`) — less mental
rewriting. It's gated on `NOTE_EXTRAS` (default on; stripped by
`NOTE_MINIMAL`, where you must use `.noteId` directly).

### `hub.sync()` vs `hub.sync.status()`

`nc.hub.sync()` returns a `HubSync` request. `nc.hub.syncStatus()` returns
a `HubSyncStatus` request. Don't write `nc.hub.sync().status()` — that
doesn't compile because `HubSync` has no `.status()` method.

### Debug output

note-c's `setDebugOutputStream(Serial)` has a direct equivalent:

```cpp
// note-cpp: enable wire tracing (prints all JSON sent/received)
nc.setDebugOutput(Serial);

// With additional categories (timing, transport events):
nc.setDebugOutput(Serial, note::DebugWire | note::DebugTiming);
```

### Missing response fields

Some Notecard responses include fields not yet in the typed response
struct (e.g. `web.put` response includes `length` and `cobs` when
`binary: true`). These are upstream spec gaps — the fields exist on the
wire but aren't in the schema. Workaround: use `card.binary.status()`
to get size info after a web transaction, or use the raw request escape
hatch (`nc.request(...)`) for full JSON access. Report missing fields
as issues so they can be added to the spec.

