# JSON Backend

The Notecard uses JSON as its wire format. From your application's
perspective this is an implementation detail — you work with
meaningfully typed request and response structs, and `note-cpp`
handles the JSON details internally.

A `JsonBackend` selects the **JSON layer** strategy: it tells the
Notecard to use *tree mode* for response parsing — every response is
parsed into a `JsonReader` tree that you can walk via
`response.body()` after the call returns. Without a `JsonBackend` the
Notecard runs in *sink mode* instead: SAX events fire directly into
the response sink, no intermediate tree, and `.into(T&)` is the way
to capture body data.

> See [Transport / JSON layer](transport.md#json-layer-the-actual-bufferedstreaming-choice)
> for the full mode comparison. Both modes give you the same typed
> API surface (`api.note.read().into(struct).execute()`,
> `nc.transact(json, buf)`, etc.) — the difference is whether
> `response.body()` returns a walkable tree or `nullptr`.

**Short answer:** you almost never need to choose a backend.
`CjsonBackend` (cJSON tree, heap-allocated) works everywhere and is
the right default. Only read further if one of these applies:

- You want sink mode (no JsonBackend) for the lowest-memory profile.
- You're memory-constrained and need to avoid heap allocation during
  serialisation but still want tree mode (`StaticJsonBackend` /
  `CjsonArenaBackend`).
- You already have a JSON library linked (cJSON, nlohmann-json) and
  want to reuse it instead of pulling in a second one.
- You're debugging wire traffic and want a tree-based backend because
  it's easier to inspect in a debugger.

If none of those apply, skip to [Configuration](#configuration) and
move on.

## Which backend should I use?

| Situation | Backend | Heap? |
|-----------|---------|-------|
| Getting started / prototyping | `CjsonBackend` (default) | Yes |
| Memory-constrained, no JsonReader needed | *(none — sink mode)* | No |
| Memory-constrained, want tree, no external deps | `StaticJsonBackend<N,T>` | No |
| Memory-constrained, prefer debuggable tree | `CjsonArenaBackend` | No (arena) |
| Already using cJSON (ESP-IDF, note-c) | `CjsonBackend` | Yes |
| Already using nlohmann-json | `NlohmannBackend` | Yes |

For sink mode (no backend), construct the Notecard with the
streaming-only ctor:

```cpp
note::Notecard nc(transport, note::Allocator{});  // no backend → sink mode
```

`.into(T&)` populates user structs in this mode just as it does in
tree mode. `response.body()` returns `nullptr`.

## Configuration

### Default — `CjsonBackend`

```cpp
#include <note/backends/cjson.hpp>

note::backends::CjsonBackend backend;
note::Notecard nc(backend, transport);
```

Uses [cJSON](https://github.com/DaveGamble/cJSON) to build and parse
JSON as an in-memory tree. Familiar, inspectable in a debugger, and
often already linked (ESP-IDF bundles cJSON; `note-c` uses it).

**Tradeoff:** multiple small heap allocations per request/response —
one per JSON node.

### Zero-heap — `StaticJsonBackend`

```cpp
#include <note/backends/buffer.hpp>

note::backends::StaticJsonBackend<512, 64> backend;
note::Notecard nc(backend, transport);
```

Builds JSON into a fixed `char` buffer and parses responses using
[jsmn](https://github.com/zserge/jsmn) tokens. Both buffers live
inside the backend object — no heap, no external JSON library.

Template parameters:

- `512` — request build buffer, in bytes. Must fit your largest
  request.
- `64` — maximum jsmn token count. A flat `{"key":"value"}` with *N*
  fields uses roughly `1 + 2*N` tokens.

The backend object is large-ish (a little over 512 + 64×16 bytes).
Place it on the stack, as a static, or as a member of your app
struct — whatever suits your memory layout.

**Tradeoff:** no tree to inspect when debugging. Stick to
source-level debugging or log the raw JSON.

### Zero-heap with a tree — `CjsonArenaBackend`

```cpp
#include <note/backends/cjson.hpp>

uint8_t buf[2048];
note::MonotonicArena arena(buf, sizeof(buf));
note::backends::CjsonArenaBackend backend(arena);
note::Notecard nc(backend, transport);
```

cJSON's tree structure backed by a fixed-size arena instead of the
heap. You get the debuggability of a tree *and* bounded memory. The
arena resets automatically between requests.

### `NlohmannBackend`

```cpp
#include <note/backends/nlohmann.hpp>

note::backends::NlohmannBackend backend;
note::Notecard nc(backend, transport);
```

Uses [nlohmann/json](https://github.com/nlohmann/json). Only
worthwhile when your project already depends on it — the binary size
is large for embedded targets.

## Custom backends

If you need to use a JSON library not listed above, implement the
`JsonBackend` interface in
[`include/note/json.hpp`](../include/note/json.hpp):

```cpp
class JsonBackend {
public:
    virtual std::unique_ptr<JsonBuilder> create_builder() = 0;
    virtual std::unique_ptr<JsonReader> parse_response(string_view json) = 0;
    virtual JsonBuilder& get_builder();  // optional: reuse a member builder
    virtual ~JsonBackend() = default;
};
```

`JsonBuilder` serializes typed fields to JSON; `JsonReader` extracts
typed fields from a JSON string. The bundled backends are good
implementation references.
