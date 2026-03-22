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
// ../examples/migration_notec.cpp#L125-L136

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
```

</td><td>

```cpp
// ../examples/arduino-migration/src/main.cpp#L108-L114

auto r = nc.card.temp().read().execute();
if (r) {
    double temp = r.value;
    Serial.println(temp);
} else {
    Serial.println(r.error());
}





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

J *rsp = nc.requestAndResponse(
    nc.newRequest("card.version"));
if (rsp != NULL) {
    char *ver = JGetString(rsp, "version");
    char *dev = JGetString(rsp, "device");
    Serial.println(ver);
    nc.deleteResponse(rsp);
}
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

// Arm for connectivity + motion triggers
J *req = nc.newRequest("card.attn");
JAddStringToObject(req, "mode",
    "arm,connected,motion");
JAddNumberToObject(req, "seconds", 120);
nc.sendRequest(req);

// Disarm
req = nc.newRequest("card.attn");
JAddStringToObject(req, "mode",
    "disarm,-all");
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
    char *payload = JGetString(rsp, "payload");
    if (payload && payload[0]) {
        // Resume from saved state
    }
    nc.deleteResponse(rsp);
}
```

</td><td>

```cpp
// Sleep — save state across reset
CardAttn::Sleep req;
req.seconds(1_hours);
req.payload("checkpoint-v1");
nc.execute(req);
// Enter deep sleep...

// On wake — retrieve saved state
auto r = nc.execute(
    CardAttn::Retrieve{});
if (r && r.time != 0) {
    auto payload = r.payload; // "checkpoint-v1"
    // Resume from saved state
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

Or with [embedded-config-cpp](https://github.com/m-mcgowan/embedded-config-cpp)
for schema-based config:

```cpp
// Define once — loaded from Notehub env vars
struct AppConfig {
    int sync_interval = 60;
};
template<>
struct ec::Schema<AppConfig> {
    static inline auto fields = std::tuple{
        ec::field("interval", &AppConfig::sync_interval)
            .range(1, 3600),
    };
};

// ConfigManager handles parsing, validation,
// defaults, and change notifications.
ec::NotecardProvider<AppConfig> prov(api);
ec::ConfigManager<AppConfig> config;
config.add_provider(prov);
config.load();
auto interval = config.config().sync_interval; // int, validated
```

</td></tr>
</table>

**Key differences:**
- No manual string-to-int conversion (`atoi`). With embedded-config-cpp,
  field types are declared in the schema — parsing, validation, and type
  conversion happen automatically.
- Change detection, defaults, and observer notifications are built in.
  In note-c, you poll `env.modified` and re-parse everything yourself.

## Error handling

<table>
<tr><th>note-arduino</th><th>note-cpp</th></tr>
<tr><td>

```c
// ../examples/migration_notec.cpp#L212-L223

J *rsp = nc.requestAndResponse(
    nc.newRequest("card.version"));
if (rsp == NULL) {
    Serial.println("no response");
} else if (nc.responseError(rsp)) {
    char *err = JGetString(rsp, "err");
    Serial.println(err);
    nc.deleteResponse(rsp);
} else {
    // use response...
    nc.deleteResponse(rsp);
}
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
| `Destructive` | Consumes or deletes data (e.g. `note.get` pop) | Only if `Error::SendFailed` |

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
requests are safe to retry.

## Fire-and-forget commands

<table>
<tr><th>note-arduino</th><th>note-cpp</th></tr>
<tr><td>

```c
// ../examples/migration_notec.cpp#L228-L231

// newCommand sends "cmd" not "req" — Notecard
// doesn't respond. Easy to confuse with newRequest.
J *req = nc.newCommand("hub.sync");
nc.sendRequest(req);
```

</td><td>

```cpp
// ../examples/arduino-migration/src/main.cpp#L199

nc.hub.sync().command();



```

</td></tr>
</table>

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

7. **Replace `env.get` + `atoi` with embedded-config-cpp** if you have more
   than a couple of environment variables. Schema-based config handles parsing,
   validation, defaults, and change detection in one place.
