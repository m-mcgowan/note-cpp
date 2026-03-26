# Body Values and Note Templates

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

### 3. JsonBuf with runtime values

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

### 4. Builder lambda

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

### 5. Raw JSON string

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

Not all bodies have a fixed structure. Sensor readings may include
optional fields depending on what's available — GPS when locked,
battery when low, etc. This is a key Notecard feature.

### JsonBuf (recommended for conditional content)

```cpp
note::JsonBuf<128> body;
body.add("temp", temp);
if (have_gps) {
    body.add("lat", lat);
    body.add("lon", lon);
}
if (battery < 20) {
    body.add("low_battery", true);
}
body.close();

nc.note.add().file("sensors.qo").body(body.view()).execute();
```

### Builder lambda

```cpp
nc.note.add()
    .file("sensors.qo")
    .body(note::body([&](note::JsonBuilder& b) {
        b.add("temp", temp);
        if (have_gps) {
            b.add("lat", lat);
            b.add("lon", lon);
        }
        if (battery < 20) {
            b.add("low_battery", true);
        }
    }))
    .execute();
```

Both approaches guarantee valid JSON regardless of which branches
execute. Keys are always quoted, commas always correct, objects always
closed. The Notecard accepts any body shape — no schema registration
needed (though [templates](#template-registration) optimize bandwidth
when the shape is known).

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

Parse a response body back into your struct with `bodyAs<T>()`:

```cpp
auto r = api.note.get().get().file("data.qi").execute();
if (r) {
    Readings data = r.bodyAs<Readings>();
    printf("temp=%.1f humidity=%d\n", data.temperature, data.humidity);
}
```

## Template registration

Notecard [templates](https://dev.blues.io/notecard/notecard-walkthrough/low-bandwidth-design/#notecard-templates) optimize bandwidth by sending only values, not field names. `note-cpp` auto-generates the template definition from your struct:

```cpp
api.note.template_().set("sensors.qo")
    .body(note::template_of<Readings>())
    .execute();
```

`template_of<Readings>()` produces the Notecard type hints: `14.1` (TFLOAT32) for `float`, `11` (TINT16) for `int16_t`. The mapping is:

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

On C++20+, plain aggregates work automatically via structured bindings. On C++17, you need the `NOTE_FIELDS` macro to tell the library about your fields:

```cpp
struct Readings {
    float temperature;
    int16_t humidity;
    NOTE_FIELDS(temperature, humidity)
};
```

The macro generates the reflection metadata needed for serialization, deserialization, and template generation.

## NTN considerations

When using NTN (satellite), templates should use `compact` format and specify a port (1-100) for efficient over-the-air encoding. The [app orchestration layer](app-orchestration.md) handles this automatically when NTN mode is enabled.

See [examples/sending-notes/](../examples/sending-notes/) for a complete walkthrough of all body patterns.
