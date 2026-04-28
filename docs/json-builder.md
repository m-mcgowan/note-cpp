# JSON Buffer Builder

`note::JsonBuf` builds JSON into a fixed-size buffer with zero allocations. When all values are compile-time constants, the entire JSON string is computed at compile time by the compiler. Replace a `constexpr` value with a runtime variable and the same API works at runtime — no code changes needed.

## Compile-time JSON

### Auto-sized

The `note::json<>()` helper measures the output at compile time and picks the exact buffer size:

```cpp
constexpr auto req = note::json<[](auto& b) {
    b.add("req", "hub.set");
    b.add("product", "com.example.app");
    b.add("mode", "periodic");
    b.add("outbound", 60);
    b.close();
}>();

// Verified at compile time — zero runtime cost
static_assert(req.view() ==
    R"({"req":"hub.set","product":"com.example.app","mode":"periodic","outbound":60})");
```

### Fixed-size buffer

For explicit control over buffer size:

```cpp
constexpr note::JsonBuf<128> body = [] {
    note::JsonBuf<128> b;
    b.add("req", "note.add");
    b.add("file", "sensors.qo");
    b.begin_object("body");
        b.add("temp", 22.5);
    b.end_object();
    b.close();
    return b;
}();

static_assert(body);  // overflow check — fails at compile time if buffer too small
```

## Compile-time `env.default`

The same pattern suits any request with hardcoded values. Fallback
defaults that the firmware registers at startup are a common case:

```cpp
constexpr auto default_interval = note::json<[](auto& b) {
    b.add("req", "env.default");
    b.add("name", "interval");
    b.add("text", "300");
    b.close();
}>();

// Send it via nc.send(json) or nc.transact(json, buf) — the unified
// raw-JSON API works in either JSON-layer mode (tree or sink). No
// builder, no allocator, no runtime formatting — the entire string
// lives in flash.
```

See [environment-variables.md](environment-variables.md) for the full
env var API and
[`examples/stdcpp/env-vars.cpp`](../examples/stdcpp/env-vars.cpp) for
this pattern running alongside the typed API.

## Wire-name escape hatch

The typed API renames wire keys that collide with C++ reserved words or
the `note::` namespace (see the [rename table in
environment-variables.md](environment-variables.md#wire-names-vs-c-names)
— `note` → `.noteId`, `delete` → `.delete_`, etc.). The JsonBuf builder
takes the wire key verbatim, so if you want to spell it literally — or
you're building a request the typed API doesn't yet cover — this is the
way:

```cpp
// The typed API would call this field .noteId; the wire key is "note".
constexpr auto note_add = note::json<[](auto& b) {
    b.add("req", "note.add");
    b.add("file", "data.db");
    b.add("note", "custom-note-id");   // wire key, unrenamed
    b.begin_object("body");
        b.add("temp", 22.5);
    b.end_object();
    b.close();
}>();
```

Same applies to `"delete"`, `"class"`, `"template"`, or any other key
that the codegen had to rename for C++ safety. The JsonBuf path isn't
type-checked against the OpenAPI spec — you're responsible for the
field names — but it lets you emit any JSON the Notecard will accept.

## Runtime usage

The same API works at runtime. Just remove `constexpr`:

```cpp
float temperature = read_sensor();

auto body = note::json<[](auto& b) {
    b.add("req", "note.add");
    b.add("file", "sensors.qo");
    b.close();
}>();

// Or with runtime values:
note::JsonBuf<128> buf;
buf.add("req", "note.add");
buf.add("file", "sensors.qo");
buf.begin_object("body");
    buf.add("temp", temperature);  // runtime value
buf.end_object();
buf.close();
```

## `JsonBuf` API

| Method | Description |
|--------|-------------|
| `add(key, value)` | Add a key-value pair (bool, int32_t, double, string_view) |
| `begin_object(key)` | Start a nested JSON object |
| `end_object()` | Close the current object |
| `begin_array(key)` | Start a JSON array |
| `end_array()` | Close the current array |
| `close()` | Close the root object (must call before reading) |
| `view()` | Get the JSON as `string_view` |
| `data()` | Raw `const char*` pointer (null-terminated) |
| `size()` | Length of the JSON string |
| `operator bool()` | `false` if the buffer overflowed |

## Overflow handling

If the buffer is too small, the `JsonBuf` sets an internal overflow flag. No undefined behavior — the buffer simply stops accepting data:

```cpp
note::JsonBuf<16> tiny;
tiny.add("key", "this string is way too long for 16 bytes");
tiny.close();
assert(!tiny);  // overflow detected
```

At compile time, overflow causes a compile error via `static_assert`.

## When to use

`JsonBuf` is ideal for:
- **Static requests** that never change (constexpr hub.set, card.version, etc.)
- **Low-level transport** where you want zero-allocation JSON
- **Embedded targets** with no heap, although the out of the box experience makes this unnecessary.

For most application code, use the [typed API](../README.md#generated-api-types) instead — it handles JSON building internally and gives you type safety on top.
