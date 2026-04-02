# Working with Responses

Every `execute()` call returns an `ApiResult<Response>` — a result type
that either holds a typed response or an error. This guide covers how to
read response fields, access nested objects and arrays, parse body data,
and handle the common patterns.

## Basic response reading

```cpp
auto r = nc.card.version().execute();
if (!r) {
    // Transport or Notecard error
    Serial.print("Error: ");
    Serial.println(r.error().message);
    return;
}

// Access typed fields directly
Serial.print("Version: ");
Serial.println(r.version);     // ResponseField<string_view> — Printable
Serial.print("Device: ");
Serial.println(r.device);

// Print the entire response as JSON (Arduino)
Serial.println(r);
```

Response fields are `ResponseField<T>` which implicitly converts to `T`.
On Arduino, every field and every response is `Printable` —
`Serial.print()` works directly.

## Checking for fields

Response fields are always present (no `std::optional`). Missing fields
get their type's default: empty string_view, 0, false. Check presence
with the reader if needed:

```cpp
auto r = nc.card.temp().read().execute();
if (r) {
    float temp = r.value;        // 0.0 if field absent
    int32_t calibration = r.calibration;  // 0 if field absent
}
```

## Void responses

Some endpoints return no data — just success or error:

```cpp
auto r = nc.hub.set().product("com.example").execute();
if (!r) {
    // Error — the Notecard returned {"err":"..."}
}
// No fields to read — r is just a success/fail indicator
```

These use `ApiResult<void>` which has `operator bool()` and `.error()`.

## Array response fields

Some responses include JSON arrays (e.g., `card.attn` query returns a
`files` array showing which triggers fired). These are parsed into
`ResponseArray<T, N>`:

```cpp
auto r = nc.card.attn().query().execute();
if (r) {
    Serial.print("ATTN set: ");
    Serial.println(r.set ? "yes" : "no");

    // Iterate the files that triggered ATTN
    for (auto& file : r.files) {
        Serial.print("  trigger: ");
        Serial.println(file);
    }
}
```

`ResponseArray` is a fixed-capacity inline array (no heap). It supports
range-for, `size()`, `operator[]`, and `begin()`/`end()`.

## Body responses — nested objects

Endpoints like `note.get` return a `body` field containing user data
(sensor readings, configuration). Access it with `body()` (raw reader)
or `bodyAs<T>()` (typed parse):

### Raw body access

```cpp
auto r = nc.note.get().read().file("sensors.qi").execute();
if (r && r.body()) {
    auto* body = r.body();  // const JsonReader*
    float temp = body->get_double("temperature");
    int32_t humidity = body->get_int("humidity");
}
```

### Typed body parsing (recommended)

Define a struct matching the body shape:

```cpp
struct Readings {
    float temperature;
    int16_t humidity;
    NOTE_FIELDS(temperature, humidity)  // C++17; not needed on C++20+
};

auto r = nc.note.get().read().file("sensors.qi").execute();
if (r) {
    auto data = r.bodyAs<Readings>();
    Serial.print("Temp: ");
    Serial.print(data.temperature);
    Serial.print(" Humidity: ");
    Serial.println(data.humidity);
}
```

The `NOTE_FIELDS` macro enables reflection-based parsing on C++17.
On C++20+, aggregate structs are reflected automatically — no macro
needed:

```cpp
struct Readings {
    float temperature;
    int16_t humidity;
};
```

### Body arrays

When the body contains arrays, access them through the raw reader:

```cpp
auto r = nc.note.get().read().file("config.db").execute();
if (r && r.body()) {
    // Array of strings
    note::string_view tags[8];
    size_t n = r.body()->get_string_array("tags", tags, 8);
    for (size_t i = 0; i < n; ++i) {
        Serial.println(tags[i]);
    }
}
```

## Response lifetimes

Response fields are views into the transport buffer. For `string_view`
fields, the view is valid until the next `execute()` call on the same
Notecard. For non-string fields (int, bool, double), the value is copied.

```cpp
auto r = nc.card.version().execute();
auto ver = r.version;   // valid now

nc.hub.set().execute();  // transport buffer reused
// ver is now dangling — don't read it
```

To extend string lifetimes, use an arena allocator:

```cpp
char pool[256];
note::MonotonicArena arena(pool);
nc.set_allocator(note::arena_allocator(arena));

auto r = nc.card.version().execute();
auto ver = r.version;  // interned into arena — survives buffer reuse
```

See [Response Lifetimes](response-lifetimes.md) for details.

## Error responses

The Notecard signals errors with `{"err":"message"}`. The typed API
captures this as an `ErrorInfo`:

```cpp
auto r = nc.card.version().execute();
if (!r) {
    auto& err = r.error();
    Serial.print("Code: ");
    Serial.println(static_cast<int>(err.code));
    Serial.print("Message: ");
    Serial.println(err.message);
}
```

Error codes distinguish transport failures (`SendFailed`, `ResponseLost`)
from Notecard errors (`Notecard`). See [Error Handling](error-handling.md).

## The raw escape hatch

For responses the typed API doesn't fully cover, use `BareNotecard` for
raw JSON passthrough:

```cpp
note::BareNotecard bare(transport);
char buf[512];
auto rsp = bare.transact(R"({"req":"card.version"})", buf);
if (rsp) {
    // *rsp is the raw JSON response string
    Serial.println(*rsp);
}
```

Or on a `Notecard` that also uses the typed API:

```cpp
char buf[512];
auto rsp = nc.transact(R"({"req":"card.version"})", buf);
```
