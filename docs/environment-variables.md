# Environment variables

The Notecard keeps a key/value store of *environment variables* that a
device can read to fetch runtime configuration without reflashing firmware.
Values can be set at the device, fleet, or project level in Notehub, and
a full precedence hierarchy resolves which value wins on any given read.

For a conceptual background — what env vars are, where they live, the
hierarchy, reserved `_`-prefix system variables - please see

- [**Understanding Environment Variables**](https://dev.blues.io/guides-and-tutorials/notecard-guides/understanding-environment-variables/) — Blues developer guide.
- [**env Requests API reference**](https://dev.blues.io/api-reference/notecard-api/env-requests/) — wire-protocol reference for `env.get`, `env.set`, `env.default`, `env.modified`, `env.template`.

This page focuses on the **C++ patterns** for using env vars from
`note-cpp` and the various modes of `env.get`, streaming into a
struct, the JSON-layer trade-offs between streaming mode and tree
mode, and the compile-time behaviour of the typed API.

See [`examples/stdcpp/env-vars.cpp`](../examples/stdcpp/env-vars.cpp) for a full runnable example.

## 1. Read a single variable

Fastest when you know exactly one value you want. The response's
`text` field holds the value.

```cpp
auto r = nc.env.get().name("region").execute();
if (r) {
    note::string_view region = r.text.value();
    note::json_int_t  when   = r.time;  // store's last-modified time
}
```

Wire form:

```json
{"req":"env.get","name":"region"}
→ {"text":"us-east-1","time":1700000000}
```

## 2. Read multiple variables into a struct

Define a struct whose field names match the env-var names, pass it to
`.into(cfg)`. The response body streams directly into the struct —
SAX-based, no intermediate tree.

```cpp
struct DeviceConfig {
    note::string_view region;
    note::string_view locale;
    note::json_int_t  interval;
    NOTE_FIELDS(region, locale, interval)
};

DeviceConfig cfg{};
auto req = nc.env.get();
req.names = {"region", "locale", "interval"};   // see note below
auto r = req.into(cfg).execute();
if (r) {
    // cfg.region, cfg.locale, cfg.interval populated
}
```

**`req.names({...})` doesn't chain.** `names` is an `ArrayField`, and
its `operator()` returns the array, not the request. Set it via
assignment (`req.names = {...}`), chained `.add()` calls
(`req.names.add("x").add("y")`), or direct assignment to a braced list
— then continue the request chain with `.into(cfg).execute()` on the
request itself.

Wire form:

```json
{"req":"env.get","names":["region","locale","interval"]}
→ {"body":{"region":"us-east-1","locale":"en-US","interval":300},"time":1700000000}
```

Fields on the struct without a matching body key are left at their
default-constructed value — so always default-init the struct
(`DeviceConfig cfg{}`) before parsing.

## 3. Read every variable

Omit `names` to get everything the Notecard knows about. The struct
pattern still works — matching keys fill in, extras are ignored.

```cpp
DeviceConfig cfg{};
auto r = nc.env.get().into(cfg).execute();
```

Wire form:

```json
{"req":"env.get"}
→ {"body":{"region":"us-east-1","locale":"en-US","interval":300,"debug":"false"},"time":1700000000}
```

## 4. Poll for changes

Pair a remembered timestamp with `.time(t)`. The Notecard still returns
body + current `time`, but you can compare `r.time` against your saved
timestamp to decide whether to act.

```cpp
note::json_int_t last_seen = saved_from_previous_poll();
auto req = nc.env.get();
req.names = {"region", "locale", "interval"};
req.time  = last_seen;
DeviceConfig cfg{};
auto r = req.into(cfg).execute();
if (r && static_cast<long long>(r.time) > last_seen) {
    // changed — apply cfg
}
```

For a cheap *"did anything change?"* poll without parsing the body,
use `env.modified` instead:

```cpp
auto r = nc.env.modified().execute();
if (r && static_cast<long long>(r.time) > last_seen) {
    // fetch cfg with env.get
}
```

## Setting values from the host

- **`env.default(name, text)`** — register a host-side fallback that
  the Notecard uses only when nothing else sets that variable.
- **`env.set(name).text(value)`** — authoritative host override. Wins
  over Notehub's synced values.

```cpp
nc.env.setDefault("interval", "300").execute();      // fallback
nc.env.set("debug").text("true").execute();           // override
```

Note: Blues deprecated `env.set` in Notecard firmware v7.2.2. note-cpp
still exposes it for firmware compatibility, but prefer setting values
from Notehub or via `env.default` for new code. See
[env Requests](https://dev.blues.io/api-reference/notecard-api/env-requests/)
for details.

### Hardcoded defaults at compile time

When the default value is known at build time (e.g. a firmware-wide
fallback), bake the JSON into flash via `note::json<lambda>()`:

```cpp
constexpr auto default_interval = note::json<[](auto& b) {
    b.add("req", "env.default");
    b.add("name", "interval");
    b.add("text", "300");
    b.close();
}>();
static_assert(default_interval.view() ==
    R"({"req":"env.default","name":"interval","text":"300"})");
```

Zero runtime cost — the string is measured and emitted at compile time.
See [json-builder.md](json-builder.md) for the full `JsonBuf` /
`json<>` API including the runtime-values variant.

## Dynamic body keys (`env.get` with all variables)

`.into(struct)` and tree-mode `r.body()` cover most response shapes — see [working-with-responses.md § Body responses](working-with-responses.md#body-responses--nested-objects) for the canonical treatment. For `env.get` with no `name` argument the body keys aren't known at compile time, so `.into(struct)` doesn't apply. Either walk a tree-mode `JsonReader` by key, or — for the streaming-mode / no-backend path — use `nc.transact()` plus `note::scan::*`:

```cpp
char buf[256];
auto rsp = nc.transact("{\"req\":\"env.get\"}", note::span<char>(buf));
if (rsp) {
    auto body = note::scan::object(*rsp, "body");
    note::scan::for_each(body, [](note::string_view k, note::string_view v) {
        // each top-level body field — keys here don't have to be known
        // at compile time
    });
}
```

On AVR-class builds (`NOTE_MINIMAL` sets `NOTE_NO_JSON_TREE=1`), tree mode is compiled out — the `transact` + `scan` pattern above is the only option for dynamic keys.

## C++ level

- **C++17** — everything above works, with `NOTE_FIELDS(...)` as the
  body-struct reflection macro.
- **C++20** — the `NOTE_FIELDS(...)` macro becomes optional for
  aggregate body structs (compile-time reflection takes over).
- Field identifier mismatches (e.g. `cfg.regoin` instead of
  `cfg.region`) are **compile errors** in all cases. The struct fields
  and the request/response types are all typed members, so typos never
  silently parse to nothing.

## Wire names vs C++ names

env.get doesn't rename any fields, but some endpoints do — the
codegen has a small table of renames for reserved-word safety:

| Wire key    | C++ field / method |
|-------------|--------------------|
| `note`      | `noteId` *(used by `note.get`, `note.read`)* |
| `delete`    | `delete_`          |
| `template`  | `template_`        |
| `class`     | `class_`           |
| `new`       | `new_`             |

These are enforced at compile time (you can't type `.note(...)` on a
request whose field is `.noteId`), and always round-trip to the
original wire keys in the JSON you send.
