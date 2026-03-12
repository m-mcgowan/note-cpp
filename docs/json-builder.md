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

## API

| Method | Description |
|--------|-------------|
| `add(key, value)` | Add a key-value pair (bool, int32_t, double, string_view) |
| `begin_object(key)` | Start a nested JSON object |
| `end_object()` | Close the current object |
| `begin_array(key)` | Start a JSON array |
| `end_array()` | Close the current array |
| `close()` | Close the root object (must call before reading) |
| `view()` | Get the JSON as `string_view` |
| `data()` | Raw `const char*` pointer |
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
- **Embedded targets** with no heap

For most application code, use the [typed API](../README.md#generated-api-types) instead — it handles JSON building internally and gives you type safety on top.
