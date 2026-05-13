# Migrating from note-c (host)

This guide is for projects running [note-c](https://github.com/blues/note-c) directly on a Linux/macOS host or an embedded RTOS — i.e. **not** through note-arduino. (If you're on Arduino, use [migration-from-note-arduino.md](../arduino/migration-from-note-arduino.md) instead — note-arduino wraps note-c, so the patterns there cover that case.) Each section pairs a representative note-c snippet with the `note-cpp` equivalent, ending at the bridge transport that lets you migrate one request at a time.

`note-cpp` and note-c can coexist on the same wire — there's a [bridge transport](#bridge-mode-incremental-migration) (`examples/stdcpp/note-c-bridge.cpp`) that lets `note-cpp`'s typed API ride on note-c's existing serial/I2C link, so you can adopt the typed API without rewriting your transport setup. Many migration patterns are platform-agnostic; rather than re-explain them here, this doc cross-links to the [Arduino migration guide](../arduino/migration-from-note-arduino.md) which has the deeper side-by-side coverage.

## Setup

In note-c, you call `NoteSetFn*` to register transport function pointers and then talk to the Notecard with `NoteRequest`/`NoteRequestResponseJSON`:

```c
#include "note.h"

// Wire up your I2C/serial implementation by passing
// function pointers to NoteSetFnI2C / NoteSetFnSerial.
NoteSetFnI2C(NOTE_I2C_ADDR_DEFAULT, NOTE_I2C_MAX_DEFAULT,
             my_i2c_reset, my_i2c_transmit, my_i2c_receive);

J *req = NoteNewRequest("hub.set");
JAddStringToObject(req, "product", "com.example.app");
JAddStringToObject(req, "mode", "periodic");
NoteRequest(req);
```

In `note-cpp` on a host, you instantiate a HAL, a transport, a JSON backend, and a `Notecard`/`Api` pair:

```cpp
#include <note/notecard.hpp>
#include <note/api.hpp>
#include <note/backends/cjson.hpp>
#include <note/link/serial.hpp>
#include <note/protocol.hpp>

MySerialHal hal;                              // your HAL impl (note::SerialHal)
note::link::SerialFramer serial_hal(hal);     // wraps to note::Hal — wire framing
note::Protocol transport(serial_hal);         // ITransact — wire protocol
note::backends::CjsonBackend backend;         // JSON backend (cJSON-backed)

note::Notecard nc(backend, transport);
note::Api api(nc);

api.hub.set().product("com.example.app").mode("periodic").execute();
```

There are no global function pointers — every Notecard owns its transport and backend. See [`docs/getting-started.md` § stdcpp / CMake host](../../getting-started.md#stdcpp-cmake-host) for the canonical setup walkthrough.

> **POSIX shortcut.** On Linux/macOS/BSD, `#include <note/posix.hpp>` collapses the four-object setup to `note::posix::Notecard nc; nc.begin("/dev/ttyUSB0", 9600);` — see [`examples/stdcpp/posix-hardware.cpp`](../../../examples/stdcpp/posix-hardware.cpp) for a runnable example that talks to real hardware over USB-serial or I2C.

## Bridge mode (incremental migration)

If your existing project already wires `NoteSetFnI2C`/`NoteSetFnSerial` correctly and you don't want to rewrite the transport layer, you can plug `note-cpp`'s typed API on top of note-c's transport. The bridge implements `note::ITransact` by delegating each request to note-c's `NoteRequestResponseJSON`:

```cpp
extern "C" char* NoteRequestResponseJSON(const char* reqJSON);
```

The bridge transport itself is short — it forwards the JSON request string to note-c, copies the response into the caller's buffer, and exposes a no-op HAL because note-c owns the actual hardware:

```cpp
/// Delegates every request to note-c's NoteRequestResponseJSON so note-c
/// owns the serial/I2C bus and note-cpp sits on top with its typed API.
class NoteCTransport : public note::ITransact {
    std::string rsp_buf_;
public:
    using note::ITransact::transact;
    using note::ITransact::send;

    note::Result<note::string_view> transact(note::string_view req,
                                             note::span<char> buf, uint32_t) override {
        std::string req_str(req.data(), req.size());
        char* rsp = NoteRequestResponseJSON(req_str.c_str());
        if (rsp == nullptr) {
            return note::make_error(note::Error::ResponseLost, "no response");
        }
        rsp_buf_ = rsp;
        std::free(rsp);
        if (rsp_buf_.size() >= buf.size())
            return note::make_error(note::Error::Overflow, NOTE_ERR("response exceeds buffer"));
        std::memcpy(buf.data(), rsp_buf_.data(), rsp_buf_.size());
        return note::string_view(buf.data(), rsp_buf_.size());
    }
    // ... send(), reset(), abort(), and a no-op HAL elided
};
```

Wire it up the same way as a real transport (the runnable example uses `MockBackend` so it compiles standalone; in your project, swap in `CjsonBackend` or another real backend):

```cpp
int main() {
    MockBackend backend;            // → note::backends::CjsonBackend in your project
    NoteCTransport transport;
    note::Notecard nc(backend, transport);
    note::Api api(nc);

    // Typed API calls route through note-c's existing transport.
    api.hub.set().product("com.example.app").mode("periodic").execute();
    return 0;
}
```

The full runnable file is at [`examples/stdcpp/note-c-bridge.cpp`](../../../examples/stdcpp/note-c-bridge.cpp).

**Migration arc.** Bridge mode is for the middle phase of a migration:

1. **Drop in `note-cpp`** behind the bridge — keep all your `NoteSetFn*` setup, link both libraries, and wire the bridge transport. Existing `J*`/`NoteRequest` code keeps working unchanged because note-c still owns the bus.
2. **Migrate request sites one by one** to typed `note-cpp` calls. Both styles share the wire, so half-migrated code is fine.
3. **Cut over the transport** when you're done — replace the bridge with a real `note-cpp` `Hal`/transport, drop the note-c link, and delete the `NoteSetFn*` calls. At this point you stop linking note-c entirely.

You can stop at step 2 indefinitely if linking both libraries is acceptable. Bridge mode adds one indirection per request and a bounce through note-c's allocator; cutover removes both.

## Request mapping

Five canonical Notecard endpoints, side by side. Where the typed API has a focused operation (e.g. `note.read` vs `note.pop`), it's shown explicitly. Calling style examples are the fluent chain — direct assignment and designated initializers (C++20) work the same way; see the [Arduino migration guide § API Styles](../arduino/migration-from-note-arduino.md#api-styles).

### hub.set — configure sync mode

<table>
<tr><th>note-c</th><th>note-cpp</th></tr>
<tr><td>

```c
J *req = NoteNewRequest("hub.set");
JAddStringToObject(req, "product",
    "com.example.app");
JAddStringToObject(req, "mode", "periodic");
JAddNumberToObject(req, "outbound", 60);
NoteRequest(req);
```

</td><td>

```cpp
api.hub.set()
    .product("com.example.app")
    .mode("periodic")
    .outbound(60_mins)
    .execute();
```

</td></tr>
</table>

`60_mins` is type-safe — passing a raw integer or `60_s` where minutes are expected is a compile error. See [duration-units.md](../../duration-units.md). On C++20, the literal `"periodic"` is also validated against the enum at compile time.

### note.add — send sensor data

<table>
<tr><th>note-c</th><th>note-cpp</th></tr>
<tr><td>

```c
J *req = NoteNewRequest("note.add");
JAddStringToObject(req, "file", "sensors.qo");
J *body = JAddObjectToObject(req, "body");
JAddNumberToObject(body, "temperature", 22.5);
JAddNumberToObject(body, "humidity", 60);
NoteRequest(req);
```

</td><td>

```cpp
struct Readings {
    float temperature;
    int16_t humidity;
    NOTE_FIELDS(temperature, humidity)  // optional on C++20
};

Readings r{.temperature = 22.5f, .humidity = 60};
api.note.add()
    .file("sensors.qo")
    .body(r)
    .execute();
```

</td></tr>
</table>

The same `Readings` struct is reusable for receive (`.into(r)`) and template registration (`note::template_of(r)`) — define your shape once, use it everywhere.

### note.get / note.pop — read by ID vs queue drain

<table>
<tr><th>note-c</th><th>note-cpp</th></tr>
<tr><td>

```c
// Read by ID (non-destructive)
J *req = NoteNewRequest("note.get");
JAddStringToObject(req, "file", "data.db");
JAddStringToObject(req, "note", "my-id");
J *rsp = NoteRequestResponse(req);

// Pop from queue (destructive)
J *req2 = NoteNewRequest("note.get");
JAddStringToObject(req2, "file", "data.qi");
JAddBoolToObject(req2, "delete", true);
J *rsp2 = NoteRequestResponse(req2);
```

</td><td>

```cpp
// Read by ID — note.get without delete
auto r = api.note.read("data.db")
    .noteId("my-id")
    .execute();

// Pop from queue — note.get with delete
auto r2 = api.note.pop("data.qi").execute();
```

</td></tr>
</table>

Both produce `note.get` on the wire. `read()` can't accidentally include `delete:true`; `pop()` always does. Each variant only exposes the fields that apply. See [Focused operations](../../using-the-api.md#focused-operations-on-multi-purpose-endpoints).

### card.attn — arm for interrupts

<table>
<tr><th>note-c</th><th>note-cpp</th></tr>
<tr><td>

```c
// Build the comma-joined mode by hand —
// typo or wrong order fails silently.
J *req = NoteNewRequest("card.attn");
JAddStringToObject(req, "mode",
    "arm,connected,motion");
JAddNumberToObject(req, "seconds", 120);
NoteRequest(req);
```

</td><td>

```cpp
// Named methods — "arm," prefix is built
// for you; misspellings won't compile.
api.card.attn().arm()
    .connected()
    .motion()
    .seconds(120_s)
    .execute();
```

</td></tr>
</table>

`Arm`, `Rearm`, `Disarm`, `Sleep`, `Watchdog`, `Off`, `On` are distinct types — each exposes only the fields that apply to that operation, so you can't accidentally set `payload` on a disarm or `start` on an arm. See [the Arduino guide's ATTN sections](../arduino/migration-from-note-arduino.md#attn-pin-arming-for-interrupts) for sleep/retrieve patterns and array-field iteration; the ATTN API surface is identical on host.

### env.get — read an environment variable

<table>
<tr><th>note-c</th><th>note-cpp</th></tr>
<tr><td>

```c
J *req = NoteNewRequest("env.get");
JAddStringToObject(req, "name", "interval");
J *rsp = NoteRequestResponse(req);
if (rsp != NULL) {
    char *text = JGetString(rsp, "text");
    int interval = atoi(text);
    NoteDeleteResponse(rsp);
}
```

</td><td>

```cpp
auto r = api.env.get().name("interval").execute();
if (r) {
    auto text = r.text;   // string_view
    // for integers, parse text yourself, or
    // use the multi-into-struct mode below
}
```

</td></tr>
</table>

`env.get` has four modes (single, multi-named, all, change-polling). Multi-named with body-into-struct parsing — the most useful host pattern — is covered in [`docs/environment-variables.md`](../../environment-variables.md) and [`examples/stdcpp/env-vars.cpp`](../../../examples/stdcpp/env-vars.cpp).

## Response handling

note-c gives you a `J *` tree to walk; note-cpp gives you a typed struct that lives on the stack. Common patterns:

| note-c | note-cpp |
|---|---|
| `J *rsp = NoteRequestResponseJSON(...)` | `auto r = api.x.y().execute();` |
| `if (rsp == NULL)` (no response / timeout) | `if (!r)` and inspect `r.error()` |
| `JGetString(rsp, "field")` | `r.field` (typed `string_view` or numeric) |
| `JGetNumber(rsp, "field")` | `r.field` |
| `JIsPresent(rsp, "field")` | `r.field.has_value()` (where applicable) |
| `JDelete(rsp)` | nothing — RAII cleans up at scope exit |
| `nc.responseError(rsp)` | `r.error()` returns a structured `ErrorInfo` (`code`, `cause`, `message`) |

`r.error()` is structured: `Error::SendFailed` (never reached the Notecard, safe to retry), `Error::Notecard` (the Notecard returned an error in the `err` field), `Error::Json` (parse failure). See [error-handling.md](../../error-handling.md) for the full reference including retry safety levels.

**Body parsing.** Where note-c hands you a `J *body = JGetObject(rsp, "body")` and you walk it field by field, note-cpp parses directly into a struct during the streaming SAX pass — no intermediate buffer, no allocation:

```cpp
Readings r;
api.note.read("data.db").noteId("my-id").into(r).execute();
// r.temperature, r.humidity are populated by the time execute() returns
```

See [working-with-responses.md](../../working-with-responses.md) for the full picture (presence checks, lifetimes, body parsing). String-field lifetimes are the one footgun worth knowing about — `string_view` fields are valid until the next `execute()` unless you intern them through an arena. See [response-lifetimes.md](../../response-lifetimes.md).

## Build setup

`note-cpp` is header-only and ships a CMake config. Add it as a subdirectory and link:

```cmake
add_subdirectory(third_party/note-cpp)
target_link_libraries(my_app PRIVATE note-cpp)
target_compile_features(my_app PRIVATE cxx_std_20)   # 17 is the minimum; 20 unlocks more
```

If you're cutting over from note-c entirely, you can drop:

- `note-c/` from your build's source list and the `-lnote` (or equivalent) link line
- `cJSON.c`/`cJSON.h` if your project pulled them in alongside note-c — `note-cpp`'s `CjsonBackend` reuses ESP-IDF's bundled cJSON or vendors its own; see [json-backend.md](../../json-backend.md) for available backends (cJSON, nlohmann::json, fixed-buffer)
- `NoteSetFn*` initialization — replaced by passing `Hal`/transport/backend instances to the `Notecard` constructor

If you're staying in [bridge mode](#bridge-mode-incremental-migration), keep linking note-c — the bridge calls `NoteRequestResponseJSON` directly.

C++ standard: C++17 minimum, C++20 unlocks designated initializers, `consteval` enum-string validation, and a few API niceties. See [`getting-started.md` § ESP-IDF](../../getting-started.md#esp-idf) for ESP-IDF-specific component config.

## What's deliberately different

A migrator's "things that aren't 1:1" list. None of these are bugs — they're shape changes you'll notice in the first hour of porting code:

- **No global state.** note-c keeps the active transport in static `NoteSetFn*` pointers; `note-cpp`'s `Notecard` owns its transport. Two `Notecard`s on two buses just work — no swapping.
- **Field names sometimes differ between wire and library.** The wire field name (e.g. `note`, `delete`) is what the Notecard sees; the C++ method/field name (e.g. `noteId`, `delete_`) is what your code reads. Reserved C++ keywords get the underscore suffix; ambiguous names get spelled out. The wire name is always shown in IntelliSense / generated docs.
- **`hub.sync` is async by default in both libraries**, but note-cpp's `Result<void>` distinguishes "send queued" (`Error::SendFailed` is absent) from "request name didn't even leave the host" — note-c collapses these.
- **No `J *` tree to materialize.** note-c builds requests/responses as a `J *` tree, then serialises to/from wire JSON internally. note-cpp builds JSON in a single buffer pass; the raw escape hatch (`nc.transact(json_string, buf)`) takes JSON text directly.
- **String lifetimes are explicit.** `string_view` fields in responses point into the response buffer and become invalid on the next `execute()`. note-c's strings have the same problem (they live inside the `J *` tree until you `JDelete` it), but the lifetime rule is more visible in C++. See [response-lifetimes.md](../../response-lifetimes.md).
- **Errors are structured, not stringly-typed.** note-c gives you the Notecard's `err` string (`"note.add: queue full"`); note-cpp gives you that string plus a `code`/`cause` so you can branch on category without parsing English. See [error-handling.md](../../error-handling.md).
- **Templates are derived from your struct.** note-c asks you to register `note.template` separately and keep `TFLOAT32`/`TINT16` constants in sync with your fields. note-cpp derives the template from the same struct you use for `body()`/`into()` — `note::template_of(Readings())`.

Beyond this list, most behavioural differences are platform-agnostic and live in the [Arduino migration guide](../arduino/migration-from-note-arduino.md) — the API surface is the same.

## See also

- [`examples/stdcpp/note-c-bridge.cpp`](../../../examples/stdcpp/note-c-bridge.cpp) — runnable bridge transport example
- [`examples/stdcpp/posix-hardware.cpp`](../../../examples/stdcpp/posix-hardware.cpp) — POSIX shortcut talking to real Notecard hardware over serial/I2C
- [`docs/platforms/arduino/migration-from-note-arduino.md`](../arduino/migration-from-note-arduino.md) — Arduino migration (more comprehensive; many patterns are platform-agnostic)
- [`docs/getting-started.md`](../../getting-started.md) — start here for greenfield projects (the stdcpp/CMake host section is the host setup canonical)
- [`docs/using-the-api.md`](../../using-the-api.md) — API styles, focused operations, escape hatches
- [`docs/error-handling.md`](../../error-handling.md) — `Result<T>`, `ErrorInfo`, retry safety
- [`docs/response-lifetimes.md`](../../response-lifetimes.md) — `string_view` validity, arena interning
