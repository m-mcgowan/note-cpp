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

Response fields track whether they were present in the JSON response.
Use `has_value()` to distinguish absent fields from zero/empty values:

```cpp
auto r = nc.card.attn().retrieve().execute();
if (r) {
    if (r.time.has_value()) {
        Serial.print("Time: ");
        Serial.println(r.time);
    } else {
        Serial.println("time field not in response");
    }

    // Reading a field that wasn't present returns the type default (0, false, "")
    int32_t t = r.time;  // 0 if absent — same as before
}
```

For string fields, `empty()` is often sufficient since the Notecard
doesn't send empty strings for absent fields:

```cpp
auto r = nc.card.version().execute();
if (!r.version.empty()) {
    Serial.println(r.version);
}
```

You can disable presence tracking with `NOTE_RESPONSE_PRESENCE=0` to
save a byte per field. When disabled, `has_value()` always returns true.

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
(sensor readings, configuration). Use `.into()` on the request to parse
the body directly into a struct during streaming, or `body()` for raw
reader access on the buffered path.

### Typed body parsing (recommended)

Define a struct matching the body shape, and pass it to `.into()`:

```cpp
struct Readings {
    float temperature;
    int16_t humidity;
    NOTE_FIELDS(temperature, humidity)  // C++17; not needed on C++20+
};

Readings data;
auto r = nc.note.read("sensors.qi").into(data).execute();
if (r) {
    Serial.print("Temp: ");
    Serial.print(data.temperature);
    Serial.print(" Humidity: ");
    Serial.println(data.humidity);
}
```

The body is parsed during the SAX streaming pass — primitive fields
are written directly into the struct with zero arena cost. String
fields are interned into the arena.

The `NOTE_FIELDS` macro enables reflection-based parsing on C++17.
On C++20+, aggregate structs are reflected automatically — no macro
needed:

```cpp
struct Readings {
    float temperature;
    int16_t humidity;
};
```

### Raw body access (buffered path)

On the buffered parse path, the raw `JsonReader` is still available:

```cpp
auto r = nc.note.read("sensors.qi").execute();
if (r && r.body()) {
    auto* body = r.body();  // const JsonReader*
    float temp = body->get_double("temperature");
    int32_t humidity = body->get_int("humidity");
}
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

## Object arrays in responses

Some responses include arrays of objects (e.g., `card.aux` returns a
`state` array of GPIO pin states). Access these through the reader's
`get_object_array()`:

```cpp
auto r = nc.card.aux().execute();
if (r) {
    auto* reader = ...;  // get the response reader
    std::unique_ptr<note::JsonReader> pins[8];
    size_t n = reader->get_object_array("state", pins, 8);
    for (size_t i = 0; i < n; ++i) {
        bool high = pins[i]->get_bool("high");
        bool low = pins[i]->get_bool("low");
        bool input = pins[i]->get_bool("input");
    }
}
```

For simple cases, you can also use `nc.transact()` to get the raw JSON
and parse it yourself:

```cpp
char buf[512];
auto r = nc.transact(R"({"req":"card.aux"})", buf);
if (r) {
    // *r is the raw JSON — parse state array as needed
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
