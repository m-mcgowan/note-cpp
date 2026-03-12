# Body Values and Note Templates

Notecard Notes carry a JSON body — sensor readings, configuration, user data. `note-cpp` supports three tiers of body handling, from simplest to most powerful.

## Three tiers

### Tier 1: Raw JSON string

Pass a JSON string directly. No type checking, but convenient for one-off cases:

```cpp
api.noteAdd()
    .file("sensors.qo")
    .body(R"({"temp":22.5,"humidity":60})")
    .execute();
```

### Tier 2: Builder lambda

Use `note::body()` with a lambda for type-safe building without defining a struct:

```cpp
api.noteAdd()
    .file("sensors.qo")
    .body(note::body([](note::JsonBuilder& b) {
        b.add("temp", 22.5);
        b.add("humidity", int32_t{60});
    }))
    .execute();
```

### Tier 3: Typed struct (recommended)

Define a struct once. Use it to **send** data, **receive** data, and **register templates**. The same type does all three.

```cpp
struct Readings {
    float temperature;
    int16_t humidity;
    NOTE_FIELDS(temperature, humidity)   // C++17; not needed on C++20+
};
```

## Sending typed bodies

Both fluent and assignment styles work:

```cpp
// Fluent — inline
api.noteAdd()
   .file("sensors.qo")
   .body(Readings{.temperature = 22.5f, .humidity = 60})
   .execute();

// Assignment
Readings r;
r.temperature = 22.5f;
r.humidity = 60;
auto req = api.noteAdd();
req.file = "sensors.qo";
req.body(r);
req.execute();
```

## Receiving typed bodies

Parse a response body back into your struct with `bodyAs<T>()`:

```cpp
auto r = api.noteGet().get().file("data.qi").execute();
if (r) {
    Readings data = r.bodyAs<Readings>();
    printf("temp=%.1f humidity=%d\n", data.temperature, data.humidity);
}
```

## Template registration

Notecard [templates](https://dev.blues.io/notecard/notecard-walkthrough/low-bandwidth-design/#notecard-templates) optimize bandwidth by sending only values, not field names. `note-cpp` auto-generates the template definition from your struct:

```cpp
api.noteTemplate().set("sensors.qo")
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
