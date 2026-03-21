# Migrating from note-arduino (note-c) to note-cpp

If you're coming from the [note-arduino](https://github.com/blues/note-arduino)
library (which wraps [note-c](https://github.com/blues/note-c)), this guide
shows how your existing code maps to note-cpp. Each section shows the note-c
pattern on the left and the note-cpp equivalent on the right.

The short version: note-c builds JSON by hand with string keys and manual
memory management. note-cpp gives you typed fields, IDE autocomplete, and
compile-time error checking — the same requests on the wire, but the compiler
catches mistakes before they reach the device.

## Setup

<table>
<tr><th>note-arduino</th><th>note-cpp</th></tr>
<tr><td>

```c
#include <Notecard.h>

Notecard notecard;

void setup() {
    notecard.begin(Serial1, 9600);
}

```

</td><td>

```cpp
#include <note/note.hpp>

note::Notecard nc;
note::Api api(nc);

void setup() {
    nc.begin(Serial1, 9600);
}
```

</td></tr>
</table>

**What changed:** One include, same `begin()` call. The JSON backend and
transport are handled internally — the only visible difference is
`note::Api`, which gives you the typed request builders on top.

## Configuring the product (hub.set)

<table>
<tr><th>note-arduino</th><th>note-cpp</th></tr>
<tr><td>

```c
J *req = notecard.newRequest("hub.set");
JAddStringToObject(req, "product",
    "com.example.app");
JAddStringToObject(req, "mode", "periodic");
JAddNumberToObject(req, "outbound", 60);
notecard.sendRequest(req);
```

</td><td>

```cpp
api.hub.set()
    .product("com.example.app")
    .mode("periodic")
    .outbound(60)
    .execute();

```

</td></tr>
</table>

**What changed:**
- No `J*` pointers to manage — no risk of leaking a request object.
- Field names are typed members. `JAddStringToObject(req, "prodcut", ...)` compiles
  fine in note-c but fails silently; `req.prodcut` in note-cpp won't compile.
- `outbound` is an integer in note-c — you have to remember it's in minutes.
  note-cpp accepts `60_mins` or `1_hours` with type-safe duration units that
  prevent accidental mixing of minutes and seconds.
- On C++20, `"periodic"` is validated at compile time. A typo like `"perioidc"`
  is a compile error.

## Sending sensor data (note.add)

<table>
<tr><th>note-arduino</th><th>note-cpp</th></tr>
<tr><td>

```c
J *req = notecard.newRequest("note.add");
JAddStringToObject(req, "file", "sensors.qo");
J *body = JAddObjectToObject(req, "body");
JAddNumberToObject(body, "temp", temperature);
JAddNumberToObject(body, "humidity", humidity);
notecard.sendRequest(req);







```

</td><td>

```cpp
// Define your data shape once:
struct Readings {
    float temperature;
    int16_t humidity;
    NOTE_FIELDS(temperature, humidity) // optional on C++20
};

// Then send it:
Readings r{.temperature = 22.5f, .humidity = 60};
api.note.add()
    .file("sensors.qo")
    .body(r)
    .execute();
```

</td></tr>
</table>

**What changed:**
- The body is a typed struct, not a hand-built JSON tree. The same struct is
  used to send data, receive data, and register Notecard templates.
- No two-level pointer management (`req` → `body` → fields). If you forget
  to add the `body` object in note-c, the fields end up on the request itself
  and are silently ignored. With note-cpp, `.body(r)` is explicit.
- `JAddNumberToObject` sends everything as `double`. note-cpp preserves the
  original type (`float`, `int16_t`) so template registration generates the
  correct type hints automatically.

## Registering templates

<table>
<tr><th>note-arduino</th><th>note-cpp</th></tr>
<tr><td>

```c
// Magic numbers: 14.1 = TFLOAT32, 11 = TINT16
// Get one wrong and the Notecard stores garbage.
J *req = notecard.newRequest("note.template");
JAddStringToObject(req, "file", "sensors.qo");
J *body = JAddObjectToObject(req, "body");
JAddNumberToObject(body, "temp", 14.1);
JAddNumberToObject(body, "humidity", 11);
notecard.sendRequest(req);
```

</td><td>

```cpp
// Same Readings struct from above — type hints
// are derived from C++ types automatically.
api.note.templates().define("sensors.qo")
    .body(note::template_of<Readings>())
    .execute();



```

</td></tr>
</table>

**What changed:**
- No magic numbers. `14.1` meaning "32-bit float" is a Notecard convention that
  note-c requires you to memorize. note-cpp derives the type hints from your
  struct: `float` → `14.1`, `int16_t` → `11`, `bool` → `1`, etc.
- One struct for everything. Define `Readings` once, and the library uses it
  for sending, receiving, and template registration.

## Reading temperature (card.temp)

<table>
<tr><th>note-arduino</th><th>note-cpp</th></tr>
<tr><td>

```c
J *rsp = notecard.requestAndResponse(
    notecard.newRequest("card.temp"));
if (rsp != NULL) {
    double temp = JGetNumber(rsp, "value");
    Serial.println(temp);
    notecard.deleteResponse(rsp);
}
```

</td><td>

```cpp
auto r = api.card.temp().read().execute();
if (r) {
    auto temp = r.value;
    Serial.println(temp);
}
// no manual cleanup needed

```

</td></tr>
</table>

**What changed:**
- No manual response lifecycle (`deleteResponse`). The response is an
  RAII value that cleans up automatically.
- `JGetNumber(rsp, "value")` returns 0.0 on misspelling with no error.
  `r.value` is a named member — misspelling won't compile.
- `card.temp` is a polymorphic API in the Notecard: it can read the current
  temperature or configure periodic monitoring depending on which fields you
  send. note-c uses one function for both; note-cpp has `.read()` and
  `.configure()` — each with only the fields that apply.

## Reading device info (card.version)

<table>
<tr><th>note-arduino</th><th>note-cpp</th></tr>
<tr><td>

```c
J *rsp = notecard.requestAndResponse(
    notecard.newRequest("card.version"));
if (rsp != NULL) {
    char *ver = JGetString(rsp, "version");
    char *dev = JGetString(rsp, "device");
    Serial.println(ver);
    notecard.deleteResponse(rsp);
}
```

</td><td>

```cpp
auto r = api.card.version().execute();
if (r) {
    auto ver = r.version;
    auto dev = r.device;
    // string_views — valid until next execute()
}


```

</td></tr>
</table>

**What changed:**
- `JGetString` returns a `char*` that you must not use after `deleteResponse`.
  note-cpp returns `string_view` with the same lifetime constraint, but you
  never have to think about `deleteResponse` — it happens automatically.
- `r.version` and `r.device` are typed members, not string lookups.

## ATTN pin — arming for interrupts

<table>
<tr><th>note-arduino</th><th>note-cpp</th></tr>
<tr><td>

```c
// Set up ATTN to watch for file changes
J *req = notecard.newRequest("card.attn");
const char *files[] = {"my-inbound.qi"};
J *arr = JCreateStringArray(files, 1);
JAddItemToObject(req, "files", arr);
JAddStringToObject(req, "mode", "files");
notecard.sendRequest(req);

// Later: arm with a timeout
J *req = notecard.newRequest("card.attn");
JAddStringToObject(req, "mode", "reset");
JAddNumberToObject(req, "seconds", 120);
notecard.sendRequest(req);

// Disarm
J *req = notecard.newRequest("card.attn");
JAddStringToObject(req, "mode", "disarm,-files");
notecard.sendRequest(req);
```

</td><td>

```cpp
// Arm for specific triggers — the "arm,"
// prefix and mode string are built for you.
api.card.attn().arm()
    .files()
    .connected()
    .seconds(120_s)
    .execute();

// Disarm
api.card.attn().disarm().execute();








```

Or using the intent-based types:

```cpp
nc.execute(
    note::api::CardAttn::Arm{}
        .files().connected());

nc.execute(note::api::CardAttn::Disarm{});
```

</td></tr>
</table>

**What changed:**
- No manual string concatenation for mode flags. In note-c, you build
  `"arm,files,connected"` yourself — get the commas or names wrong and it
  fails silently. note-cpp has named methods (`.files()`, `.connected()`)
  and flag constants (`note::attn::arm | note::attn::files`).
- Intent-based types (`Arm`, `Disarm`, `Sleep`, `Watchdog`) expose only the
  fields relevant to that operation. You can't accidentally set a sleep
  timeout on a disarm request.

## ATTN pin — sleep with state

<table>
<tr><th>note-arduino</th><th>note-cpp</th></tr>
<tr><td>

```c
// Sleep — save state across reset
J *req = notecard.newCommand("card.attn");
JAddStringToObject(req, "mode", "sleep");
JAddNumberToObject(req, "seconds", 3600);
JAddStringToObject(req, "payload", "checkpoint-v1");
notecard.sendRequest(req);
// Enter deep sleep...

// On wake — retrieve saved state
J *req = notecard.newRequest("card.attn");
JAddBoolToObject(req, "start", true);
J *rsp = notecard.requestAndResponse(req);
if (rsp != NULL) {
    char *payload = JGetString(rsp, "payload");
    if (payload && payload[0]) {
        // Resume from saved state
    }
    notecard.deleteResponse(rsp);
}
```

</td><td>

```cpp
// Sleep — save state across reset
note::api::CardAttn::Sleep req;
req.seconds(1_hours);
req.payload("checkpoint-v1");
nc.execute(req);
// Enter deep sleep...

// On wake — retrieve saved state
auto r = nc.execute(
    note::api::CardAttn::Retrieve{});
if (r && r.time != 0) {
    auto payload = r.payload; // "checkpoint-v1"
    // Resume from saved state
}





```

</td></tr>
</table>

**What changed:**
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
J *req = notecard.newRequest("env.get");
JAddStringToObject(req, "name", "interval");
J *rsp = notecard.requestAndResponse(req);
if (rsp != NULL) {
    char *text = JGetString(rsp, "text");
    int interval = atoi(text);
    notecard.deleteResponse(rsp);
}

// Set a default
J *req = notecard.newRequest("env.default");
JAddStringToObject(req, "name", "interval");
JAddStringToObject(req, "text", "60");
notecard.sendRequest(req);
```

</td><td>

```cpp
// Read a single env var
auto r = api.env.get()
    .name("interval")
    .execute();
if (r) {
    auto text = r.text;  // "60"
}

// Set a default
api.env.setDefault("interval", "60")
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

**What changed:**
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
J *rsp = notecard.requestAndResponse(
    notecard.newRequest("card.version"));
if (rsp == NULL) {
    Serial.println("no response");
} else if (notecard.responseError(rsp)) {
    char *err = JGetString(rsp, "err");
    Serial.println(err);
    notecard.deleteResponse(rsp);
} else {
    // use response...
    notecard.deleteResponse(rsp);
}
```

</td><td>

```cpp
auto r = api.card.version().execute();
if (!r) {
    auto err = r.error();
    // err.code:    Error::Notecard, SendFailed, etc.
    // err.cause:   Cause::Timeout, HalError, etc.
    // err.message: human-readable string
    Serial.println(to_string(err).c_str());
} else {
    // use r.version, r.device, etc.
}


```

</td></tr>
</table>

**What changed:**
- Structured errors with code, cause, and message — not a bare string.
  `Error::SendFailed` tells you the request never left the host;
  `Error::Notecard` means the device returned an error. The code tells
  you whether retrying is safe.
- No null pointer checks. The result type is truthy on success, falsy
  on failure — one path, not three.
- No `deleteResponse` — cleanup is automatic.

## Fire-and-forget commands

<table>
<tr><th>note-arduino</th><th>note-cpp</th></tr>
<tr><td>

```c
// newCommand sends "cmd" not "req" — Notecard
// doesn't respond. Easy to confuse with newRequest.
J *req = notecard.newCommand("hub.sync");
notecard.sendRequest(req);
```

</td><td>

```cpp
// .command() instead of .execute() — clear intent.
api.hub.sync().command();


```

</td></tr>
</table>

## Migration checklist

1. **Replace `Notecard` with `note::Notecard` + `note::Api`.** Separate your
   JSON backend and transport — see the setup section above.

2. **Replace `J*` request building with typed API calls.** Every
   `notecard.newRequest("...")` + `JAdd*` sequence becomes `api.xxx.yyy()` +
   fluent setters or direct assignment.

3. **Replace `JGet*` response reading with typed members.** Every
   `JGetString(rsp, "field")` becomes `r.field`.

4. **Remove manual memory management.** Delete all `deleteResponse`,
   `JDelete`, null checks on `J*`. Responses are RAII values.

5. **Replace magic numbers in templates.** Replace `JAddNumberToObject(body, "temp", 14.1)`
   with `note::template_of<YourStruct>()`.

6. **Replace string constants with type-safe alternatives.**
   - `"periodic"` → `mode_t::periodic` (named constant) or validated literal
   - `60` (minutes) → `60_mins` or `1_hours` (duration types)
   - `"arm,connected"` → `.arm().connected()` (named flag methods)

7. **Replace `env.get` + `atoi` with embedded-config-cpp** if you have more
   than a couple of environment variables. Schema-based config handles parsing,
   validation, defaults, and change detection in one place.
