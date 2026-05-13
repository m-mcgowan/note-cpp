# Body values and Note templates

Notecard Notes carry a JSON body — sensor readings, configuration, user
data. `note-cpp` provides several ways to set the body, ranked from most
to least type-safe.

## Setting a body

### 1. Typed struct (recommended)

Define a struct once. Use it to **send** data, **receive** data, and
**register templates**. The same type does all three.

```cpp
struct Readings {
    float temperature;
    int16_t humidity;
    NOTE_FIELDS(temperature, humidity)   // C++17; not needed on C++20+
};
```

### 2. Compile-time JSON (C++20)

Use `note::json<>()` to build JSON at compile time. The structure is
validated by the compiler — malformed JSON won't compile:

```cpp
constexpr auto body = note::json<[](auto& b) {
    b.add("temp", 22.5);
    b.add("humidity", 60);
    b.close();
}>();

api.note.add()
    .file("sensors.qo")
    .body(body.view())
    .execute();
```

This is useful for static body content or templates where all values
are known at compile time.

### 3. json_fmt — compile-time validated template (C++20)

A printf-like JSON builder where the structure is validated at compile
time but values are inserted at runtime. No buffer size to choose, no
builder boilerplate — just write the JSON with `{}` placeholders:

```cpp
float temp = read_sensor();
nc.note.add()
    .file("sensors.qo")
    .body(note::json_fmt<R"({"temp":{},"humidity":{}})">(temp, 60).view())
    .execute();
```

Typed placeholders catch type mismatches at compile time:

```cpp
// {i}=int, {f}=float, {s}=string, {b}=bool
note::json_fmt<R"({"temp":{f},"name":{s},"active":{b}})">(22.5f, "sensor-1", true);

// Wrong type → compile error:
// note::json_fmt<R"({"count":{i}})">("not_an_int");
```

The format string itself is validated as JSON — malformed JSON, missing
braces, trailing commas, and non-object top levels are all compile errors.

At runtime, `json_fmt` is just string concatenation — no JSON parsing,
no intermediate tree, no heap allocation. This makes it efficient for
memory-constrained devices where even `JsonBuf`'s fixed buffer is a
concern. With full streaming transport, the format could be written
directly to the wire without any intermediate buffer.

### 4. JsonBuf with runtime values

`JsonBuf` works at runtime too — the builder methods guarantee valid
JSON structure (balanced braces, quoted keys, proper commas). Only the
buffer size needs specifying:

```cpp
float temp = read_sensor();
note::JsonBuf<64> body;
body.add("temp", temp);
body.add("humidity", 60);
body.close();

api.note.add()
    .file("sensors.qo")
    .body(body.view())
    .execute();
```

Unlike raw strings, `JsonBuf` can't produce malformed JSON. Keys are
always quoted, objects always closed, commas always placed correctly.

### 5. Builder lambda

Use `note::body()` with a lambda for structured building with runtime
values. The builder prevents malformed JSON (keys are always quoted,
nesting is always balanced):

```cpp
float temp = read_sensor();
api.note.add()
    .file("sensors.qo")
    .body(note::body([&](note::JsonBuilder& b) {
        b.add("temp", temp);
        b.add("humidity", int32_t{60});
    }))
    .execute();
```

### 6. Raw JSON string

Pass a pre-formed JSON string. No compile-time validation — use this
for forwarding JSON from external sources, not for authoring:

```cpp
api.note.add()
    .file("sensors.qo")
    .body(R"({"temp":22.5,"humidity":60})")
    .execute();
```

The string is embedded as a raw JSON object on the wire (not quoted
as a string value).

## Conditional / schemaless bodies

Not all bodies have a fixed structure. Sensor readings may include optional fields depending on what's available — GPS when locked, battery when low. The Notecard accepts any body shape, no schema needed (though [templates](#template-registration) optimize bandwidth when the shape *is* known).

`JsonBuf` and the builder lambda both support conditional fields — just wrap `.add()` calls in `if`:

```cpp
note::JsonBuf<128> body;
body.add("temp", temp);
if (have_gps)     { body.add("lat", lat); body.add("lon", lon); }
if (battery < 20) { body.add("low_battery", true); }
body.close();

nc.note.add().file("sensors.qo").body(body.view()).execute();
```

The builder lambda mirrors this — the `note::body([&](note::JsonBuilder& b) { ... })` form takes the same `if`-guarded `b.add(...)` calls. Both approaches guarantee balanced braces and correct comma placement regardless of which branches execute.

`json_fmt` doesn't support conditional fields — the format structure is fixed at compile time. For variable-shape bodies, use `JsonBuf` or the builder lambda.

## Sending typed bodies

Both fluent and assignment styles work:

```cpp
// Fluent — inline
api.note.add()
   .file("sensors.qo")
   .body(Readings{.temperature = 22.5f, .humidity = 60})
   .execute();

// Assignment
Readings r;
r.temperature = 22.5f;
r.humidity = 60;
auto req = api.note.add();
req.file = "sensors.qo";
req.body(r);
req.execute();
```

## Receiving typed bodies

Use `.into(struct)` on the request to parse a response body directly into the same struct you'd send. Full treatment — including the lambda-builder alternative for fields with wire-name mismatches, and tree-mode `r.body()->get_double(...)` access — lives in [working-with-responses.md § Body responses](working-with-responses.md#body-responses-nested-objects).

## Template registration

Notecard [templates](https://dev.blues.io/notecard/notecard-walkthrough/low-bandwidth-design/#notecard-templates) optimize bandwidth by sending only values, not field names. `note-cpp` auto-generates the template definition from your struct:

```cpp
api.note.templates().define("sensors.qo")
    .body(template_of(Readings()))
    .execute();
```

`template_of(Readings())` produces the Notecard type hints: `14.1` (TFLOAT32) for `float`, `11` (TINT16) for `int16_t`. The mapping is:

| C++ type | Notecard type | Template value |
|----------|--------------|----------------|
| `bool` | TBOOL | `true` |
| `int8_t` | TINT8 | `1` |
| `int16_t` | TINT16 | `11` |
| `int32_t` | TINT32 | `12` |
| `float` | TFLOAT32 | `14.1` |
| `double` | TFLOAT64 | `14.1` (same wire format) |
| `char[N]` | TSTRING(N) | `"Nnn"` (length string) |

## NOTE_FIELDS macro

On C++20+, **plain aggregates** work automatically — no macro needed:

```cpp
struct Readings {
    float temperature;
    int16_t humidity;
};
```

A plain aggregate is a struct with all public members and no user-defined
constructors. The library uses compile-time reflection to discover fields
automatically.

**When you need `NOTE_FIELDS`:**

- **C++17** — always required (no compile-time reflection available)
- **C++20 with non-aggregate structs** — if your struct has a user-defined
  constructor, it's no longer an aggregate and reflection can't inspect it.
  Add `NOTE_FIELDS` to restore support:

```cpp
// Has a constructor → not an aggregate → needs NOTE_FIELDS on C++20
struct Readings {
    float temperature;
    int16_t humidity;
    Readings() : temperature(0), humidity(0) {}
    NOTE_FIELDS(temperature, humidity)
};
```

The macro generates the serialization, deserialization, and template
metadata. It works identically on C++17 and C++20 — the only difference
is whether you can omit it for plain aggregates.

**Requirements:**
- All fields listed in `NOTE_FIELDS` must be public
- Up to 16 fields supported
- Struct must be default-constructible (response parsing calls `T{}` internally)

## NTN considerations

When using NTN (satellite), templates should use `compact` format and specify a port (1-100) for efficient over-the-air encoding. The app orchestration layer handles this automatically when NTN mode is enabled.

See [examples/sending-notes/](../examples/stdcpp/sending-notes/) for a complete walkthrough of all body patterns.
