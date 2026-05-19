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

All response string fields are backed by null-terminated storage — `.data()` is a valid C string. `string_view::size()` excludes the terminator, but `data()[size()]` is `'\0'`. Guaranteed for everything interned via `StringPool` (which is every string field on a typed response).

```cpp
auto r = nc.card.version().execute();
printf("version: %s\n", r.version.data());
for (auto& f : r.files) printf("  file: %s\n", f.data());
```

On Arduino, `Serial.print(f.data())` works as an alternative to `Serial.println(printable(f))` for array elements.

## Body responses — nested objects

Endpoints like `note.get` return a `body` field containing user data
(sensor readings, configuration). Use `.into()` on the request to parse
the body directly into a struct during streaming, or `body()` for raw
reader access on the tree path.

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

### Custom body parsing via lambda builder

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

This works on both the streaming and tree paths and gives you full
control over field-name mapping.

### Raw body access (tree path)

On the tree parse path, the raw `JsonReader` is also available
directly on the typed response:

```cpp
auto r = nc.note.read("sensors.qi").execute();
if (r && r.body()) {
    auto* body = r.body();  // const JsonReader*
    float temp = body->get_double("temperature");
    int32_t humidity = body->get_int("humidity");
}
```

In streaming mode the JSON tree is never built, so `r.body()` returns
`nullptr` — the same value it returns when the response carried no body
at all, so the two cases look identical to the caller. For portable code
that runs on either path, prefer `r.body_or_error()`, which returns an
explicit `Error::NotReady` when the Notecard parsed in streaming mode:

```cpp
auto safe = r.body_or_error();
if (safe.has_value()) {
    // safe.value() is a const JsonReader*, may still be nullptr if the
    // response had no body field
    if (auto* body = *safe) {
        float temp = body->get_double("temperature");
    }
} else {
    // safe.error().code == Error::NotReady when running streaming.
    // Use .into(MyStruct&) or .into(JsonSink&) for streaming body
    // access instead — see the previous section.
}
```

`r.was_streaming_parse()` returns the same information as a plain bool
if all you need is the path discriminator. Both helpers are gated on
the same flag the streaming `Sink` sets at construction time, so they
stay accurate even when no SAX events fire (the empty-body case).

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

Response `string_view` fields point into the transport buffer and are valid until the next `execute()` call. Non-string fields are copied. To make views outlive the next call, attach an arena — see [response-lifetimes.md](response-lifetimes.md) for sizing, the arena lifecycle, and the full set of patterns.

## Raw access

For responses the typed API doesn't fully cover — new fields the codegen doesn't know about, or one-off lookups — drop to the raw `transact()` and parse the JSON yourself:

```cpp
char buf[512];
auto rsp = nc.transact(R"({"req":"card.version"})", buf);
if (rsp) {
    // *rsp is the raw JSON response string
}
```

See [using-the-api.md § Escape hatches](using-the-api.md#escape-hatches) for the full set of escape levels (raw fields, lambda builders, fire-and-forget commands).
