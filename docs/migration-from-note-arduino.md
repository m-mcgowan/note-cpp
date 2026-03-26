# Migrating from note-arduino (note-c) to note-cpp

If you're coming from the [note-arduino](https://github.com/blues/note-arduino)
library (which wraps [note-c](https://github.com/blues/note-c)), this guide
shows how your existing code maps to note-cpp. Each section shows the note-c
pattern on the left and the note-cpp equivalent on the right.

The short version: note-c builds JSON by hand with string keys and manual
memory management. note-cpp gives you typed fields, IDE autocomplete, and
compile-time error checking — the same requests on the wire, but the compiler
catches mistakes before they reach the device.

> All note-cpp code in this guide is compiled against the real Arduino SDK
> via [examples/arduino-migration/](../examples/arduino-migration/). The
> note-c examples are compiled via
> [examples/migration_notec.cpp](../examples/migration_notec.cpp).

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
#include <note/arduino.hpp>
using namespace note::api;

note::arduino::Notecard nc;

void setup() {
    nc.begin(Serial1, 9600);
}
```

</td></tr>
</table>

> The `using namespace note::api` import is used throughout the remaining
> examples for brevity. It brings in the request types (`HubSet`, `CardAttn`,
> etc.) without affecting `nc` or other `note::` types.

## Configuring the Hub (hub.set)

<table>
<tr><th>note-arduino</th><th>note-cpp</th></tr>
<tr><td>

```c
// ../examples/migration_notec.cpp#L54-L59

J *req = nc.newRequest("hub.set");
JAddStringToObject(req, "product",
    "com.example.app");
JAddStringToObject(req, "mode", "periodic");
JAddNumberToObject(req, "outbound", 60);
nc.sendRequest(req);
```

</td><td>

```cpp
// ../examples/arduino-migration/src/main.cpp#L33-L37

nc.hub.set()
    .product("com.example.app")
    .mode("periodic")
    .outbound(60_mins)
    .execute();

```

</td></tr>
</table>

**Key differences from note-c:**
- No `J*` pointers to manage — no risk of leaking a request object.
- Field names are typed members. Misspell a field name and the compiler
  tells you — note-c compiles it silently.
- `outbound` is a bare integer in note-c — you have to remember it's in
  minutes. note-cpp accepts `60_mins` or `1_hours` with type-safe duration
  units that prevent accidental mixing of minutes and seconds.
- On C++20, `"periodic"` is validated at compile time. A typo like
  `"perioidc"` is a compile error.


The example above shows the fluent chain approach.  Note-c offers 3 primary API styles:

1. fluent chain
2. direct assignment
3. designated initializers

### Fluent chain

Build and execute in one expression. Good when all fields are known upfront, or their values may be conditional (computed by a helper function).

### Direct assignment

Set fields individually. Good when fields come from different sources or are set with inline conditional code.

<table>
<tr><th>note-arduino</th><th>note-cpp</th></tr>
<tr><td>

```c
// ../examples/migration_notec.cpp#L65-L77

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

```c
// ../examples/migration_notec.cpp#L54-L59

J *req = nc.newRequest("hub.set");
JAddStringToObject(req, "product",
    "com.example.app");
JAddStringToObject(req, "mode", "periodic");
JAddNumberToObject(req, "outbound", 60);
nc.sendRequest(req);
```

</td><td>

```cpp
// ../examples/arduino-migration/src/main.cpp#L61-L66

HubSet req{
    .mode     = "periodic",
    .outbound = 60_mins,
    .product  = "com.example.app",
};
nc.execute(req);
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

```c
// ../examples/migration_notec.cpp#L82-L93

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

```cpp
struct Readings {
    float temperature;
    int16_t humidity;
};

Readings r{.temperature = 22.5f, .humidity = 60};
nc.note.add()
    .file("sensors.qo")
    .body(r)
    .execute();
```

**With error handling:**

In note-c, you check for a null response, then check the `err` field — an
unstructured string you have to parse yourself:

```c
// ../examples/migration_notec.cpp#L99-L108

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
tells you what layer failed, the cause tells you why, and you can decide
whether retrying is safe without parsing strings:

```cpp
auto result = nc.note.add()
    .file("sensors.qo")
    .body(r)
    .execute();
if (!result) {
    auto err = result.error();
    Serial.println(err);
}
```

Possible output (Notecard error):

    notecard: note.add: queue full

Possible output (transport failure):

    send_failed[timeout]: no response within deadline

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

See [Error Handling](error-handling.md) for the full reference including
retry safety levels.

**Key differences:**
- Both sides define the same struct, but note-cpp uses it directly as the
  body — no manual field-by-field JSON construction.
- No two-level pointer management (`req` then `body`). If you forget
  `JAddObjectToObject` in note-c, the fields end up on the request itself
  and are silently ignored.
- `JAddNumberToObject` sends everything as `double`. note-cpp preserves the
  original type (`float`, `int16_t`) so template registration generates the
  correct type hints automatically.

For quick schemaless bodies without defining a struct, `json_fmt` (C++20)
is the most concise option:

```cpp
nc.note.add()
    .file("sensors.qo")
    .body(note::json_fmt<R"({"temp":{},"humidity":{}})">(temp, humidity).view())
    .execute();
```

The JSON structure is validated at compile time — malformed JSON or wrong
argument count/types are compile errors. At runtime it's just string
concatenation, no heap allocation. See [Body Values](body-values.md)
for all approaches.


## Registering templates

<table>
<tr><th>note-arduino</th><th>note-cpp</th></tr>
<tr><td>

```c
// ../examples/migration_notec.cpp#L113-L120

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

```cpp
// ../examples/arduino-migration/src/main.cpp#L99-L101

nc.note.templates().define("sensors.qo")
    .body(note::template_of(Readings{}))
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

If you are comfortable with C++ template syntax, you can also use a type-only format without requiring an instance:
```
   .body(note::template_of<Readings>())
```

## Reading temperature (card.temp)

<table>
<tr><th>note-arduino</th><th>note-cpp</th></tr>
<tr><td>

```c
// ../examples/migration_notec.cpp#L125-L141

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

```cpp
// ../examples/arduino-migration/src/main.cpp#L108-L119

auto r = nc.card.temp().read().execute();
if (r) {
    double temp = r.value;
    Serial.println(temp);
} else {
    Serial.println(r.error());
}

// Configure periodic monitoring
nc.card.temp().configure()
    .minutes(5)
    .execute();





```

</td></tr>
</table>

**Key differences:**
- Avoided manual response lifecycle (`deleteResponse`). The response is an
  RAII value that cleans up automatically. If you need to keep the response for longer, you can 
- `JGetNumber(rsp, "value")` returns 0.0 on misspelling with no error.
  `r.value` is a named member — misspelling won't compile.
- `card.temp` can do several things - it can read the current
  temperature or configure periodic monitoring depending on which fields you
  send. note-cpp has `.read()` and `.configure()` — each with only the fields that apply so the intent is clear.

## Reading device info (card.version)

<table>
<tr><th>note-arduino</th><th>note-cpp</th></tr>
<tr><td>

```c
// ../examples/migration_notec.cpp#L141-L148

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
    auto ver = r.version;
    auto dev = r.device;
    Serial.println(ver.data());
}




```

</td></tr>
</table>

**Key differences:**
- `JGetString` returns a `char*` that you must not use after `deleteResponse`.
  note-cpp returns `string_view` with the same lifetime constraint, but you
  never have to think about `deleteResponse` — it happens automatically.
- `r.version` and `r.device` are typed members, not string lookups.

## ATTN pin — arming for interrupts

<table>
<tr><th>note-arduino</th><th>note-cpp</th></tr>
<tr><td>

```c
// ../examples/migration_notec.cpp#L153-L164

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

// Disarm
nc.card.attn().disarm().execute();




```

Or using the intent-based types:

```cpp
nc.execute(
    CardAttn::Arm{}.connected());

nc.execute(CardAttn::Disarm{});
```

</td></tr>
</table>

**Key differences:**
- No manual string concatenation for mode flags. In note-c, you build
  `"arm,connected,motion"` yourself — get the commas or names wrong and it
  fails silently. note-cpp has named methods (`.connected()`, `.motion()`)
  and flag constants (`note::attn::arm | note::attn::connected`).
- Intent-based types (`Arm`, `Disarm`, `Sleep`, `Watchdog`) expose only the
  fields relevant to that operation. You can't accidentally set a sleep
  timeout on a disarm request.

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
See [Polymorphic APIs](polymorphic-apis.md) for the full list.

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

## Receiving data (bodyAs)

When reading notes with structured data, note-c returns raw JSON that
you parse field by field. note-cpp parses directly into your struct:

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
auto r = nc.note.read("data.qi")
    .execute();
if (r) {
    Readings data = r.bodyAs<Readings>();
    // data.temperature, data.humidity
    // populated from the JSON body
}

```

</td></tr>
</table>

The same `Readings` struct used for sending and template registration
also works for receiving. See [Body Values](body-values.md) for details.

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
See [Duration Units](duration-units.md) for the full type system.

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

```c
// ../examples/migration_notec.cpp#L212-L223

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

```c
// ../examples/migration_notec.cpp#L228-L231

    nc.deleteResponse(rsp);
}
}
```

</td><td>

```cpp
// ../examples/arduino-migration/src/main.cpp#L199-L199

nc.hub.sync().command();


```

</td></tr>
</table>

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

nc.binary.clear().execute();
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

**SKU targeting** (C++20) — constrain your `Api` to a specific Notecard
product. Endpoints that don't support that hardware produce warnings (or
errors in strict mode):

```cpp
note::Api<note::Product::WiFi> nc(notecard);
nc.card.wifi();   // OK — WiFi endpoint on WiFi hardware
nc.card.sleep();  // OK — universal endpoint
// nc.card.lora(); // warning: not available on WiFi
```

Neither of these has an equivalent in note-c — there, incompatible requests
compile silently and fail at runtime on the device.

## Memory and binary footprint

note-c uses `NOTE_C_LOW_MEM` to trade features for size on constrained
devices (AVR, ESP8266, Cortex-M0+, NRF51 — platforms with 2–32 KB RAM).
It's auto-detected when `float` and `double` are the same size, or set
manually. Here's what it disables:

| note-c `NOTE_C_LOW_MEM` change | Impact |
|---|---|
| Shorter error strings (`ERRSTR` macro) | Verbose messages replaced with short codes (`c_mem`, `c_err`). Saves ~200–400 bytes of `.rodata`. |
| Disable CRC validation | Entire CRC system compiled out — no corruption detection or automatic retry. Saves ~500 bytes + 22 bytes per request. |
| Disable user-agent | No device/compiler metadata sent with `hub.set`. Saves ~200 bytes code + 200–400 bytes per request payload. |
| Smaller allocation chunks | 64 bytes instead of 128 bytes — less wasted slack per `malloc`. |
| `float` instead of `double` | `JNUMBER` typedef changes. Saves 4 bytes per JSON number. Loses precision beyond ~7 significant digits. |
| Disable debug logging | `NOTE_C_LOG_DEBUG` becomes a no-op. |

note-c also has [note-c-zero](https://github.com/blues/note-c-zero),
a separate variant that uses zero static read-write memory and no
dynamic allocator.

note-cpp avoids most of these tradeoffs structurally — C++ mechanisms
replace preprocessor-driven feature stripping:

| Concern | note-c approach | note-cpp approach |
|---|---|---|
| **Heap allocation** | `malloc`/`free` with configurable chunk size | `BufferJsonBackend<N, T>` — stack-allocated build buffer and token array. No heap allocation in steady state. |
| **Response lifetime** | Caller must `deleteResponse` (leak risk) | `Allocator` / `StringPool` — arena-backed string interning. Response `string_view` fields survive transport reuse without heap allocation. |
| **Transport buffers** | `malloc`'d buffer, freed after each call | `ITransport` — transport owns a reused `std::string` buffer. No allocation after warmup. |
| **Error strings** | `ERRSTR(long, short)` macro | `string_view` literals — short by design, linker deduplicates. No long/short variants. |
| **Float precision** | `#define NOTE_C_SINGLE_PRECISION` | `json_number_t` (planned) — `std::conditional_t<sizeof(float)==sizeof(double), float, double>`. Same auto-detection, no preprocessor. |
| **Unused code** | `#ifdef` guards compile features out | `-ffunction-sections` + `--gc-sections` (default on Arduino/PlatformIO). Linker eliminates unreferenced code. |
| **CRC validation** | Disabled entirely under `NOTE_C_LOW_MEM` | Always present in `AbstractTransport`. Could be made optional via template parameter if size is critical. |
| **User-agent** | Disabled under `NOTE_C_LOW_MEM` | Planned — enabled by default, opt-out for constrained devices. |

The `zero_alloc.cpp` example demonstrates all three zero-allocation patterns
(stack buffers, arena interning, transport reuse).

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

