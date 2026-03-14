# API Design

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

Layer 2 methods live on the same group objects as Layer 1. Naming uses intent verbs rather than wire-protocol field names:

```cpp
// ── Notes ────────────────────────────────────────────────────────────────
api.note.get()                      // Layer 1: full note.get builder
api.note.read("data.qi")            // Layer 2: → note.get, file pre-set
api.note.pop("data.qi")             // Layer 2: → note.get + delete:true
api.note.delete_("x.db", "id")     // Layer 1: note.delete (separate req)
api.note.add()                      // Layer 1: note.add
api.note.changes()                  // Layer 1: full note.changes builder
api.note.popChanges("data.qi")     // Layer 2: → note.changes + delete:true

// ── Binary ───────────────────────────────────────────────────────────────
api.card.binary()                   // Layer 1: full card.binary builder
api.card.binaryStatus()             // Layer 2: → card.binary (read-only, no delete)
api.card.binaryClear()              // Layer 2: → card.binary + delete:true
api.card.binaryGet()                // Layer 1: card.binary.get (separate req)
api.card.binaryPut()                // Layer 1: card.binary.put (separate req)

// ── Temperature ──────────────────────────────────────────────────────────
api.card.temp()                     // Layer 1: full card.temp builder
api.card.readTemp()                 // Layer 2: → card.temp (read-only)
api.card.stopTemp()                 // Layer 2: → card.temp + stop:true

// ── Location ─────────────────────────────────────────────────────────────
api.card.locationMode()             // Layer 1: full card.location.mode builder
api.card.readLocationMode()         // Layer 2: → card.location.mode (read-only)
api.card.resetLocationMode()        // Layer 2: → card.location.mode + delete:true

// ── Environment variables ────────────────────────────────────────────────
api.env.default_("name")            // Layer 1: full env.default builder
api.env.setDefault("name", "text")  // Layer 2: → env.default + text pre-set
api.env.clearDefault("name")        // Layer 2: → env.default, no text (= delete)
```

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
| `card.binary()` | `card.binary` | `binaryStatus()`, `binaryClear()` |
| `card.binaryGet()` | `card.binary.get` | |
| `card.binaryPut()` | `card.binary.put` | |
| `card.carrier()` | `card.carrier` | |
| `card.contact()` | `card.contact` | `readContact()`, `setContact(...)` |
| `card.dfu()` | `card.dfu` | |
| `card.illumination()` | `card.illumination` | |
| `card.io()` | `card.io` | |
| `card.led()` | `card.led` | |
| `card.location()` | `card.location` | |
| `card.locationMode()` | `card.location.mode` | `readLocationMode()`, `resetLocationMode()` |
| `card.locationTrack()` | `card.location.track` | |
| `card.monitor()` | `card.monitor` | |
| `card.motion()` | `card.motion` | |
| `card.motionMode()` | `card.motion.mode` | |
| `card.motionSync()` | `card.motion.sync` | |
| `card.motionTrack()` | `card.motion.track` | |
| `card.power()` | `card.power` | `readPower()`, `resetPower()` |
| `card.random()` | `card.random` | |
| `card.restart()` | `card.restart` | |
| `card.restore()` | `card.restore` | |
| `card.sleep()` | `card.sleep` | |
| `card.status()` | `card.status` | |
| `card.temp()` | `card.temp` | `readTemp()`, `stopTemp()` |
| `card.time()` | `card.time` | |
| `card.trace()` | `card.trace` | |
| `card.transport()` | `card.transport` | |
| `card.triangulate()` | `card.triangulate` | |
| `card.usageGet()` | `card.usage.get` | |
| `card.usageTest()` | `card.usage.test` | |
| `card.version()` | `card.version` | |
| `card.voltage()` | `card.voltage` | `readVoltage()` |
| `card.wifi()` | `card.wifi` | |
| `card.wireless()` | `card.wireless` | |
| `card.wirelessPenalty()` | `card.wireless.penalty` | `readWirelessPenalty()`, `resetWirelessPenalty()` |

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
| `note.changes()` | `note.changes` | `popChanges(file)` |
| `note.delete_(file, id)` | `note.delete` | |
| `note.get()` | `note.get` | `read(file)`, `pop(file)` |
| `note.template_()` | `note.template` | `clearTemplate(file)` |
| `note.update()` | `note.update` | |

### `env` group

| Layer 1 method | `req` string | Layer 2 aliases |
|---|---|---|
| `env.default_("name")` | `env.default` | `setDefault(name, text)`, `clearDefault(name)` |
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
| `file.delete_()` | `file.delete` | |
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

## Migration from current API

The current API uses flat methods on `Api`:

```cpp
// Current (Layer 0 — being replaced)
api.cardVersion()                          // non-polymorphic
api.cardLocationMode().get()               // polymorphic factory
api.getCardLocationMode()                  // polymorphic flat shortcut
api.noteGet().delete_().file("data.qi")    // polymorphic factory

// New Layer 1 (matches note-python naming)
api.card.version()
api.card.locationMode()                    // full builder, all fields
api.note.get()                             // full builder, all fields

// New Layer 2 (intent-driven)
api.card.readLocationMode()
api.note.pop("data.qi")
```

The current flat methods (`cardVersion()`, `getNoteGet()`, etc.) will be retained as deprecated aliases during transition. The polymorphic factory pattern (`noteGet().get()`, `noteGet().delete_()`) is replaced by Layer 1 (full builder) + Layer 2 (intent aliases).

## Implementation plan

1. **Add `x-intents` metadata to the OpenAPI spec** for all polymorphic endpoints
2. **Generate resource group classes** (`CardGroup`, `NoteGroup`, etc.) with Layer 1 methods
3. **Generate Layer 2 intent methods** on the same group classes from `x-intents`
4. **Add group member objects** to `Api` (`api.card`, `api.note`, etc.)
5. **Deprecate current flat methods** with `[[deprecated]]` pointing to new names
6. **Update `polymorphic-apis.md`** to reference this design
7. **Update examples** to use new naming

### What stays the same

- Generated request/response types (`CardVersion`, `NoteGet::Get`, etc.) are unchanged
- `Notecard::execute()` is unchanged
- Wire protocol is unchanged
- All current code continues to compile (deprecated, not removed)

### What changes

- `Api` gains group member objects: `card`, `hub`, `note`, `env`, `file`, `dfu`, `ntn`, `var`, `web`
- Each group object has Layer 1 methods matching note-python naming
- Polymorphic endpoints get Layer 2 intent methods
- Current flat methods get `[[deprecated]]`
