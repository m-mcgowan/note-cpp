# Working with Responses

Each request returns a result that either holds a typed response or an error. This guide covers how to read response fields, check for errors, and handle common patterns.

## Reading response fields

```cpp
auto r = nc.card.version().execute();
if (r) {
    Serial.println(r.version);
    Serial.println(r.device);
}
```

Response fields are `ResponseField<T>` — they implicitly convert to `T` and are `Printable` on Arduino, so `Serial.println()` works directly.

## Error handling

```cpp
auto r = nc.card.version().execute();
if (!r) {
    Serial.println(r.error());   // prints "send_failed[timeout]: no response"
    return;
}
```

For programmatic error handling, `r.error()` returns an `ErrorInfo` with `.code`, `.cause`, and `.message`. See [error handling](error-handling.md) for details.

## Checking for fields

Response fields track whether they were present in the Notecard's response.
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
If you access response fields when the request failed, they return their type's default value (0, false, empty string).

For string fields, `empty()` is often sufficient since the Notecard
doesn't send empty strings for absent fields:

```cpp
auto r = nc.card.version().execute();
if (!r.version.empty()) {
    Serial.println(r.version);
}
```

You can disable presence tracking with `NOTE_RESPONSE_PRESENCE=0` to save a byte per field. When disabled, `has_value()` always returns true. See [feature flags](feature-flags.md) for details.

## Void responses

Some endpoints return no data — just success or error:

```cpp
auto r = nc.hub.set().product("com.example").execute();
if (!r) {
    // Error — the Notecard returned {"err":"..."}
}
// No fields to read — r is just a success/fail indicator
```
These use `ApiResult<void>` which has `operator bool()` and `.error()` but no response fields. Fire-and-forget commands (`.command()`) also return `Result<void>` with the same pattern.

## Array response fields

Some responses include JSON arrays (e.g., `card.attn` query returns a
`files` array showing which triggers fired). These are parsed into
`ResponseArray<T, N>`:

```cpp
auto r = nc.card.attn().query().execute();
if (r) {
    for (auto& file : r.files) {
        // file is a printable_string_view — works like string_view
        process(file);
    }
}
```

`ResponseArray` is a fixed-capacity inline array (no heap). It supports
range-for, `size()`, `operator[]`, and `begin()`/`end()`. String array
elements are `printable_string_view` — a `string_view` subtype that adds
`printTo()` support on Arduino.

<details><summary><strong>Arduino</strong>: printing array elements</summary>

Response fields like `r.version` are directly `Printable` via
`Serial.println()`. Array elements need the `printable()` wrapper:

```cpp
for (auto& file : r.files) {
    Serial.println(printable(file));  // printable() wraps for Serial
}
```

</details>

## String fields are null-terminated

All response string fields (`string_view`) are backed by null-terminated
storage. The `string_view::data()` pointer is a valid C string:

```cpp
auto r = nc.card.version().execute();

// Works with C string functions
printf("version: %s\n", r.version.data());
strcmp(r.device.data(), "dev:12345");

// Array elements too
for (auto& f : r.files) {
    printf("  file: %s\n", f.data());
}
```

This is guaranteed for all strings interned via the `StringPool` (which
includes every string field in typed responses). The `string_view` length
does not include the null terminator — `size()` returns the string length,
and `data()[size()]` is `'\0'`.

<details><summary><strong>Arduino</strong>: printing with data()</summary>

With null-terminated strings, `Serial.print(f.data())` works as an
alternative to `Serial.println(printable(f))` for array elements and
other bare `string_view` values:

```cpp
for (auto& f : r.files) {
    Serial.print("  file: ");
    Serial.println(f.data());  // works — null-terminated
}
```

</details>

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

### Custom Body Parsing via Lambda Builder

When the body fields do not map cleanly to a struct — for example, when
wire names differ from your variable names, or the body shape is dynamic —
use the lambda builder and parse the response via `JsonReader`:

```cpp
auto result = nc.request("note.get", [](note::JsonBuilder& b) {
    b.add("file", "sensors.qi");
});
if (result) {
    auto& rsp = *result.value();
    auto* body = rsp.get_object("body");
    if (body) {
        float temp = body->get_double("temp_c");      // wire name: "temp_c"
        int32_t rh = body->get_int("relative_humidity"); // wire name: "relative_humidity"
    }
}
```

This works on both the streaming and buffered paths and gives you full
control over field-name mapping.

### Raw Body Access (Buffered Path)

On the buffered parse path, the raw `JsonReader` is also available
directly on the typed response:

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
