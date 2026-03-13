# JSON Backend

The Notecard communicates using JSON, but `note-cpp` handles this internally — your application works with typed request and response structs, not JSON strings. The JSON backend controls how that internal serialization and parsing happens.

## Do I need to choose a backend?

**Usually not.** A default backend is provided that works out of the box. You only need to think about this if one of these applies:

- **You already have a JSON library linked** and want to avoid pulling in a second one
- **You're on a memory-constrained embedded target** and need control over where and how memory is allocated during JSON processing
- **You want to inspect the JSON** on the wire for debugging (tree-based backends make this easier)

If none of these apply, use the default and move on.

## Available backends

### cJSON

```cpp
#include <note/backends/cjson.hpp>

note::backends::CjsonBackend backend;
note::Notecard nc(backend, transport);
```

Uses the [cJSON](https://github.com/DaveGamble/cJSON) library to build and parse JSON as an in-memory tree. Good default for platforms where cJSON is already available (ESP-IDF bundles it, and it ships with `note-c`).

**Tradeoffs:** Multiple small heap allocations per request/response (one per JSON node). Familiar tree structure is easy to inspect in a debugger.

### cJSON with arena allocation

```cpp
#include <note/backends/cjson.hpp>

uint8_t buf[2048];
note::MonotonicArena arena(buf, sizeof(buf));
note::backends::CjsonArenaBackend backend(arena);
note::Notecard nc(backend, transport);
```

Same cJSON tree approach, but all allocations come from a fixed buffer you provide. The arena resets automatically between requests. Gives you the debuggability of a tree with deterministic, bounded memory use.

### nlohmann-json

```cpp
#include <note/backends/nlohmann.hpp>

note::backends::NlohmannBackend backend;
note::Notecard nc(backend, transport);
```

Uses [nlohmann/json](https://github.com/nlohmann/json). Convenient when your project already depends on it. Not recommended for constrained embedded targets due to code size.

### Zero-allocation buffer backend

```cpp
#include <note/backends/buffer.hpp>

note::backends::BufferJsonBackend<512, 64> backend;
note::Notecard nc(backend, transport);
```

No external JSON library required. Builds JSON directly into a fixed buffer and parses responses using [jsmn](https://github.com/zserge/jsmn) tokens — both are member arrays with sizes you control via template parameters.

- `512` — request build buffer size in bytes (must fit the largest JSON request your application sends)
- `64` — maximum jsmn token count (a flat `{"key":"value"}` with N fields uses roughly 1 + 2N tokens)

**Tradeoffs:** Zero heap allocation. No external dependencies (jsmn is vendored). The token array lives inside the backend object, so you control where it's placed (stack, static, heap). Less convenient for debugging since there's no inspectable tree.

## Custom backends

To use a JSON library not listed above, implement the `JsonBackend` interface in [`include/note/json.hpp`](../include/note/json.hpp):

```cpp
class JsonBackend {
public:
    virtual std::unique_ptr<JsonBuilder> create_builder() = 0;
    virtual std::unique_ptr<JsonReader> parse_response(string_view json) = 0;
    virtual JsonBuilder& get_builder();  // optional: reuse a member builder
    virtual ~JsonBackend() = default;
};
```

`JsonBuilder` serializes typed fields to JSON. `JsonReader` extracts typed fields from a JSON string. See the existing backends for implementation examples.

## Which backend should I use?

| Situation | Recommended backend |
|-----------|-------------------|
| Getting started / prototyping | Default (cJSON) |
| Already using cJSON or ESP-IDF | `CjsonBackend` |
| Already using nlohmann-json | `NlohmannBackend` |
| Memory-constrained embedded | `BufferJsonBackend` or `CjsonArenaBackend` |
| Need to inspect JSON in debugger | `CjsonBackend` (tree is visible in watch window) |
| Zero external dependencies | `BufferJsonBackend` |
| Deterministic memory, no heap | `CjsonArenaBackend` or `BufferJsonBackend` |
