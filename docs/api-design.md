# API Design

> **Internal design document.** This is for contributors working on `note-cpp` itself. If you are using the library, see the [API reference](https://blues.github.io/note-cpp) and the examples in `docs/`.

This document describes the two-layer API design for `note-cpp` and how it relates to the Notecard wire protocol and other SDKs like `note-python`.

## Background

The Notecard API is a set of JSON requests sent over serial or I2C. Each request has a `req` string (e.g. `"note.get"`) and optional fields. The original `notecard-schema` repository defines one JSON Schema file per `req` string. There are no HTTP methods in the wire protocol -- the HTTP verbs in our OpenAPI spec are a modeling convenience for dispatch metadata.

### How other SDKs work

`note-python` uses a mechanical mapping from `req` strings to Python methods, grouped by the first segment:

```python
card.version()          # {"req": "card.version"}
card.locationMode()     # {"req": "card.location.mode"}
note.get(file="data.qi")  # {"req": "note.get", "file": "data.qi"}
note.delete(file="x.db", note="id")  # {"req": "note.delete", ...}
hub.set(mode="periodic")  # {"req": "hub.set", "mode": "periodic"}
```

The naming rule: first segment becomes the module, remaining segments become a camelCase method name. No polymorphism handling -- every `req` string is a separate method, and all fields are kwargs.

### What note-cpp adds

`note-cpp` should match note-python's naming for discoverability (users coming from Python should find the same names), while adding:

- **Type safety** -- each field is a typed setter, not a generic kwarg
- **Compile-time constraints** -- polymorphic variants expose only the fields relevant to that operation
- **Safety classification** -- `ReadOnly`, `Idempotent`, `Destructive` inform retry logic
- **Target constraints** -- C++20 concepts that warn/error when an endpoint isn't available on the target SKU

## Layer 1: Wire-mapped requests

Layer 1 maps 1:1 to Notecard `req` strings, matching `note-python`'s naming. Each `req` string becomes a request builder with typed setters.

### Naming convention

The `Api` class groups methods by the first segment of the `req` string, using an intermediate object:

```cpp
note::Api api(nc);

api.card.version()           // → {"req": "card.version"}
api.card.locationMode()      // → {"req": "card.location.mode"}
api.card.binary()            // → {"req": "card.binary"}
api.card.binaryGet()         // → {"req": "card.binary.get"}
api.card.binaryPut()         // → {"req": "card.binary.put"}

api.note.get()               // → {"req": "note.get"}
api.note.delete_()           // → {"req": "note.delete"}
api.note.add()               // → {"req": "note.add"}
api.note.changes()           // → {"req": "note.changes"}
api.note.template_()         // → {"req": "note.template"}
api.note.update()            // → {"req": "note.update"}

api.hub.set()                // → {"req": "hub.set"}
api.hub.get()                // → {"req": "hub.get"}
api.hub.sync()               // → {"req": "hub.sync"}
api.hub.syncStatus()         // → {"req": "hub.sync.status"}

api.env.get()                // → {"req": "env.get"}
api.env.set("name")          // → {"req": "env.set", "name": "name"}
api.env.default_("name")     // → {"req": "env.default", "name": "name"}

api.file.changes()           // → {"req": "file.changes"}
api.file.delete_()           // → {"req": "file.delete"}
api.file.stats()             // → {"req": "file.stats"}
```

The rule: strip the first segment, camelCase the rest, append `_` if it's a C++ keyword (`delete`, `template`, `default`).

### Polymorphic endpoints in Layer 1

Some `req` strings support multiple behaviors depending on which fields are sent. In `note-python`, these are a single method accepting all kwargs. In `note-cpp`, the Layer 1 method returns the full request builder with all fields. The response is the superset of all possible fields -- the Notecard returns whichever are relevant.

```cpp
// Layer 1: full access to all fields, like note-python
auto r = api.card.temp()
    .minutes(int32_t{5})
    .execute();
// r.value, r.calibration, r.humidity, etc. — present if the Notecard returned them
```

Polymorphic endpoints and which fields control dispatch:

| `req` string | Dispatch fields | Behavior |
|---|---|---|
| `card.binary` | `delete` | Status query vs clear buffer |
| `card.contact` | `name`, `email`, `org`, `role` | Read vs write contact info |
| `card.location.mode` | `mode`, `seconds`, `lat`, `lon` / `delete` | Read vs configure vs reset GPS mode |
| `card.power` | `minutes` / `reset` | Read vs configure vs reset power tracking |
| `card.temp` | `minutes`, `status`, `sync` / `stop` | Read vs start periodic logging vs stop |
| `card.voltage` | `mode`, `set`, etc. | Read vs configure voltage monitoring |
| `card.wireless.penalty` | `set` / `reset` | Read vs configure vs reset penalty box |
| `env.default` | `text` presence | Set env default vs delete it |
| `note.changes` | `delete` | Read changes vs pop (read + delete) changes |
| `note.get` | `delete` | Read note vs pop (read + delete) note |
| `note.template` | `body` / `delete` | Set template vs clear template |

### Response schemas

The `notecard-schema` repository defines one response schema per `req` string. For polymorphic endpoints, this is a superset -- the Notecard returns whichever fields are relevant to the operation performed. For example, `card.temp` defines `{calibration, humidity, pressure, temperature, usb, value, voltage}`, but a simple read might only return `{value, calibration}`.

In `note-cpp`, the Response struct always has all fields. Missing fields use their default (0, false, empty string). This matches the JSON parsing behavior -- fields not present in the response are simply not populated.

Note: some endpoints that *appear* related are actually **separate `req` strings** with **different response schemas**:

| Appears related | Actually separate | Response difference |
|---|---|---|
| `note.get` / `note.delete` | Different `req` strings | `note.get` returns `{body, payload, time}`; `note.delete` returns `{}` |
| `card.binary` / `card.binary.get` / `card.binary.put` | Three separate `req` strings | `card.binary` returns status; `card.binary.get` returns `{err, status}` (MD5); `card.binary.put` has its own response |

These are separate Layer 1 methods, not polymorphic variants.

## Layer 2: Intent-driven overloads

Layer 2 adds semantic helper methods that map user intent to the correct wire-level operation. These are thin wrappers around Layer 1 builders with pre-set fields and constrained interfaces.

### Design principles

1. **Aliases, not abstractions** -- each Layer 2 method delegates to a Layer 1 builder. No new types, no hidden behavior.
2. **Metadata-driven** -- intent mappings come from `x-intents` metadata in the OpenAPI spec, so codegen produces them.
3. **Same response type** -- Layer 2 methods return the same Response as the underlying Layer 1 builder.
4. **Discoverable** -- grouped on the same resource objects as Layer 1 methods.

### Resource groups

Layer 2 methods live on the same group objects as Layer 1. Polymorphic endpoints use an intermediate factory object; Layer 2 shortcuts are also available directly on the group.

```cpp
// ── Notes ────────────────────────────────────────────────────────────────
api.note.get()                      // Layer 1: full note.get builder (all fields)
api.note.read("data.qi")            // Layer 2: → note.get, file pre-set
api.note.pop("data.qi")             // Layer 2: → note.get + delete:true, file pre-set
api.note.remove("x.db", "id")      // Layer 2: → note.delete, file+noteId pre-set
api.note.add()                      // Layer 1: note.add
api.note.changes().peek()           // → note.changes (read-only via factory)
api.note.popChanges("data.qi")      // Layer 2: → note.changes + delete:true, file pre-set

// ── Binary ───────────────────────────────────────────────────────────────
api.binary.status()                 // Layer 2: → card.binary (read-only)
api.binary.clear()                  // Layer 2: → card.binary + delete:true
api.card.binary().status()          // same, via card group factory
api.card.binaryGet()                // Layer 1: card.binary.get (separate req)
api.card.binaryPut()                // Layer 1: card.binary.put (separate req)

// ── Temperature ──────────────────────────────────────────────────────────
api.card.temp().read()              // → card.temp (read-only)
api.card.temp().configure()         // → card.temp (start/configure periodic logging)
api.card.temp().stop()              // → card.temp + stop:true

// ── Location ─────────────────────────────────────────────────────────────
api.card.locationMode().get()       // → card.location.mode (read current mode)
api.card.locationMode().configure() // → card.location.mode (set mode/seconds)
api.card.locationMode().remove()    // → card.location.mode + delete:true

// ── Environment variables ────────────────────────────────────────────────
api.env.defaults().set()            // Layer 1: full env.default builder (set)
api.env.setDefault("name", "text")  // Layer 2: → env.default, name+text pre-set
api.env.clearDefault("name")        // Layer 2: → env.default, name pre-set, no text (= delete)
```

### Calling forms

Layer 2 methods that take required args support three equivalent calling styles:

```cpp
// 1. Direct argument — most concise, good for literal strings
api.note.pop("data.qi").execute();

// 2. Designated initializer (C++20) — good for variables and multiple fields
api.note.read({.file = filename}).execute();
api.env.setDefault({.name = "var", .text = "value"}).execute();

// 3. Builder chaining via factory — useful when adding further options
api.note.get().pop().file("data.qi").noteId("my-note").execute();
```

The `Args` structs (`PopArgs`, `ReadArgs`, `RemoveArgs`, etc.) are also exported from the group for use with designated init. Under C++20 the overload is a duck-typed template accepting any struct with the right field names; under C++17 it accepts the concrete `Args` struct.

### Verb vocabulary

Intent verbs are chosen for clarity to Notecard users (who may not know HTTP methods):

| Verb | Meaning | Example |
|---|---|---|
| `read` | Query current state (no side effects) | `readTemp()`, `readLocationMode()` |
| `pop` | Read and delete (queue consumption) | `pop("data.qi")`, `popChanges("data.qi")` |
| `clear` | Remove/reset a configuration or buffer | `binaryClear()`, `clearDefault("name")` |
| `stop` | Stop an ongoing process | `stopTemp()` |
| `reset` | Restore to default state | `resetLocationMode()` |
| `set` | Create or update a value | `setDefault("name", "text")` |

These are paired intuitively: `set`/`clear`, `read`/`pop`, `start`/`stop`.

### Implementation

Layer 2 methods are generated from `x-intents` metadata in the OpenAPI spec:

```json
{
  "x-intents": {
    "read": { "safety": "readonly", "excludes": ["delete", "stop"] },
    "pop": { "safety": "destructive", "requires": {"delete": true}, "label": "pop" },
    "stop": { "safety": "destructive", "requires": {"stop": true}, "label": "stopTemp" }
  }
}
```

Each intent becomes a method on the resource group that:
1. Creates the Layer 1 builder
2. Pre-sets any `requires` fields
3. Returns a constrained builder that hides `excludes` fields (or the full builder if no constraints)

```cpp
// Generated code (simplified)
auto readTemp() {
    return create<api::CardTemp>()  // Layer 1 builder, all fields available
        ;  // no pre-set fields, but safety = ReadOnly
}

auto stopTemp() {
    auto r = create<api::CardTemp>();
    r.stop(true);  // pre-set the dispatch field
    return r;       // safety = Destructive
}

auto pop(string_view file) {
    auto r = create<api::NoteGet>();
    r.file(file);
    r.delete_(true);  // pre-set the dispatch field
    return r;
}
```

## Full endpoint mapping

### `card` group

| Layer 1 method | `req` string | Layer 2 aliases |
|---|---|---|
| `card.attn()` | `card.attn` | |
| `card.aux()` | `card.aux` | |
| `card.auxSerial()` | `card.aux.serial` | |
| `card.binary()` | `card.binary` | `binary.status()`, `binary.clear()` |
| `card.binaryGet()` | `card.binary.get` | |
| `card.binaryPut()` | `card.binary.put` | |
| `card.carrier()` | `card.carrier` | |
| `card.contact()` | `card.contact` | `.get()`, `.set()` |
| `card.dfu()` | `card.dfu` | |
| `card.illumination()` | `card.illumination` | |
| `card.io()` | `card.io` | |
| `card.led()` | `card.led` | |
| `card.location()` | `card.location` | |
| `card.locationMode()` | `card.location.mode` | `.get()`, `.configure()`, `.remove()` |
| `card.locationTrack()` | `card.location.track` | |
| `card.monitor()` | `card.monitor` | |
| `card.motion()` | `card.motion` | |
| `card.motionMode()` | `card.motion.mode` | |
| `card.motionSync()` | `card.motion.sync` | |
| `card.motionTrack()` | `card.motion.track` | |
| `card.power()` | `card.power` | `.read()`, `.configure()`, `.reset()` |
| `card.random()` | `card.random` | |
| `card.restart()` | `card.restart` | |
| `card.restore()` | `card.restore` | |
| `card.sleep()` | `card.sleep` | |
| `card.status()` | `card.status` | |
| `card.temp()` | `card.temp` | `.read()`, `.configure()`, `.stop()` |
| `card.time()` | `card.time` | |
| `card.trace()` | `card.trace` | |
| `card.transport()` | `card.transport` | |
| `card.triangulate()` | `card.triangulate` | |
| `card.usageGet()` | `card.usage.get` | |
| `card.usageTest()` | `card.usage.test` | |
| `card.version()` | `card.version` | |
| `card.voltage()` | `card.voltage` | `.read()`, `.configure()` |
| `card.wifi()` | `card.wifi` | |
| `card.wireless()` | `card.wireless` | |
| `card.wirelessPenalty()` | `card.wireless.penalty` | `.check()`, `.override_()`, `.clear()` |

### `hub` group

| Layer 1 method | `req` string | Layer 2 aliases |
|---|---|---|
| `hub.get()` | `hub.get` | |
| `hub.log()` | `hub.log` | |
| `hub.set()` | `hub.set` | |
| `hub.signal()` | `hub.signal` | |
| `hub.status()` | `hub.status` | |
| `hub.sync()` | `hub.sync` | |
| `hub.syncStatus()` | `hub.sync.status` | |

### `note` group

| Layer 1 method | `req` string | Layer 2 aliases |
|---|---|---|
| `note.add()` | `note.add` | |
| `note.changes()` | `note.changes` | `.peek()`, `.pop(file)` · `popChanges(file)` |
| `note.remove(file, id)` | `note.delete` | |
| `note.get()` | `note.get` | `.read()`, `.pop()` · `read(file)`, `pop(file)` |
| `note.templates()` | `note.template` | `.define(file)`, `.remove(file)` |
| `note.update()` | `note.update` | |

### `env` group

| Layer 1 method | `req` string | Layer 2 aliases |
|---|---|---|
| `env.defaults()` | `env.default` | `.set()`, `.remove(name)` · `setDefault(name, text)`, `clearDefault(name)` |
| `env.get()` | `env.get` | |
| `env.modified()` | `env.modified` | |
| `env.set("name")` | `env.set` | |
| `env.template_()` | `env.template` | |

### `file` group

| Layer 1 method | `req` string | Layer 2 aliases |
|---|---|---|
| `file.changes()` | `file.changes` | |
| `file.changesPending()` | `file.changes.pending` | |
| `file.clear()` | `file.clear` | |
| `file.delete_()` / `file.remove(file)` | `file.delete` | |
| `file.stats()` | `file.stats` | |

### Other groups

| Layer 1 method | `req` string |
|---|---|
| `dfu.get()` | `dfu.get` |
| `dfu.status()` | `dfu.status` |
| `ntn.gps()` | `ntn.gps` |
| `ntn.reset()` | `ntn.reset` |
| `ntn.status()` | `ntn.status` |
| `var.delete_()` | `var.delete` |
| `var.get()` | `var.get` |
| `var.set()` | `var.set` |
| `web.request()` | `web` |
| `web.delete_()` | `web.delete` |
| `web.get()` | `web.get` |
| `web.post()` | `web.post` |
| `web.put()` | `web.put` |

