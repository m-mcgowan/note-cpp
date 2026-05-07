# API Calling Patterns

Every Notecard API endpoint in `note-cpp` supports multiple calling styles.
A simple one-liner compiles to the same code as the verbose builder form.

## Quickstart

```cpp
note::Api nc(notecard);

// ── one-liner with positional args ─────────────────────────
api.hub.set().mode("periodic").execute();
api.note.read("data.qi").execute();
api.note.remove("data.db", "my-note").execute();
api.file.remove("old-data.db").execute();

// ── Builder pattern ─────────────────────────────────────────────────────
auto req = api.hub.set();
req.mode("periodic");
req.product("com.example.app");
req.execute();

// ── Designated initializers (C++20) ─────────────────────────────────────
api.note.read({.file = "data.qi"}).execute();
api.note.remove({.file = "data.db", .noteId = "my-note"}).execute();
api.file.remove({.files = {"a.db", "b.db"}}).execute();
```

### 1. Fluent builder

Every endpoint is a builder with typed setters that chain:

```cpp
auto result = api.hub.set()
    .mode("periodic")
    .product("com.example.app")
    .outbound(60)
    .execute();
```

Each setter returns a reference to the builder, so calls chain naturally.
`execute()` sends the request and returns a typed `ApiResult<Response>`.


### 5. Direct struct construction

For `api.execute()` with a fully constructed request:

```cpp
// C++20 designated init:
api.execute(note::api::EnvSet{.name = "temp", .text = "22.5"});

// C++17:
note::api::EnvSet req;
req.name = "temp";
req.text = "22.5";
api.execute(req);
```


### 3. Designated initializers (C++20)

Aliases accept an args struct with named fields:

```cpp
api.note.read({.file = "data.qi"}).execute();
api.env.setDefault({.name = "var", .text = "value"}).execute();
api.file.remove({.files = {"a.db", "b.db"}}).execute();
```

The `Args` structs (`ReadArgs`, `RemoveArgs`, etc.) mirror the builder's
field types. Array fields use `ArrayField`, so initializer-list syntax works
naturally inside the designator.

### 4. Args struct (C++17)

The same args struct works without designated initializers using nested braces:

```cpp
// Single field — outer braces for the struct, value inside:
api.note.read({"data.qi"}).execute();

// Multiple fields — positional order matches struct declaration:
api.note.remove({"data.db", "my-note"}).execute();

// Array field — nested braces for the initializer list:
api.file.remove({{"a.db", "b.db"}}).execute();
```


### 2. Positional shorthand

Aliases accept the most common arguments as positional parameters:

```cpp
api.note.read("data.qi")                  // file
api.note.remove("data.db", "my-note")     // file, noteId
api.env.setDefault("name", "value")       // name, text
api.file.remove("old.db")                 // files (single)
```

These return the same builder — you can chain further:

```cpp
api.note.read("data.qi").noteId("specific-note").execute();
```


## Ad-Hoc Fields (`operator[]`)

Every request supports `operator[]` for setting fields by their JSON wire
name. For known fields, the value is routed to the typed field. For
unknown fields, the value is stored in an extras buffer and serialized
alongside the typed fields.

```cpp
auto req = nc.hub.set();

// Known field — routes to the typed setter (same as req.product = "...")
req["product"] = "com.example.app";

// Unknown field — stored in extras, sent on the wire as-is
req["some_new_field"] = "value";
req["retry_count"] = int32_t(3);

req.execute();
```

This is useful when new Notecard firmware adds fields that the typed API
does not model yet, or for one-off experimentation. Supported value types
are `bool`, `int32_t`, `double`, and `string_view`.

The extras buffer holds up to 4 ad-hoc fields by default. Override
`NOTE_EXTRAS_MAX` before including any `note/api` headers to change the
limit. Define `NOTE_EXTRAS=0` to disable extras entirely and save flash.
See [Feature Flags](feature-flags.md) for details.

> **Note:** `operator[]` is available on requests only, not on responses.
> Response fields are always accessed via the typed struct members. To
> read response fields by name, use the
> [lambda builder](api-layers.md#lambda-request-builder) and parse the
> response via `JsonReader`:
>
> ```cpp
> auto result = nc.request("card.version");
> if (result) {
>     auto& reader = *result.value();
>     auto version = reader.get_string("version");
>     auto some_new_field = reader.get_int("some_new_field");
> }
> ```

## Array Fields

Some request fields accept multiple values (e.g. `file.delete` takes a list
of filenames). These support several initialization styles:

```cpp
auto req = api.file.remove();

// Initializer list — most natural for literals:
req.files = {"data.qi", "settings.db"};

// Single value — clears and sets one element:
req.files = "data.qi";

// Chained add():
req.files.add("data.qi").add("settings.db");

// Callable with initializer list:
req.files({"data.qi", "settings.db"});
```

All produce the same wire format: `"files":["data.qi","settings.db"]`.

Single-value assignment (`req.files = "data.qi"`) replaces the array contents. Use `add()` to append.

## Responses

Responses are typed structs with fields that match the Notecard's JSON output:

```cpp
auto rsp = api.card.version().execute();
if (rsp) {
    auto ver = rsp.version;      // string_view
    auto body = rsp.body;        // BodyValue (nested JSON)
}

auto rsp = api.card.temp().read().execute();
if (rsp) {
    float temp = rsp.value;      // temperature in °C
}
```
`.read()` selects the Read operation — `card.temp` is polymorphic (`Read`, `Configure`, `Stop`).

On error, `rsp` is falsy and `rsp.error()` returns the `ErrorInfo`:

```cpp
if (!rsp) {
    auto err = rsp.error();
    // err.code: Error::Notecard, Error::SendFailed, etc.
    // err.message: string_view with Notecard's error text
}
```

## C++17 vs C++20

The library requires C++17 or later standard. C++20 adds ergonomic improvements

| Feature | C++17 | C++20 |
|---------|-------|-------|
| Fluent builder | ✓ | ✓ |
| Positional shorthand | ✓ | ✓ |
| Designated initializers | — | ✓ `{.file = "x"}` |
| Args struct (nested braces) | ✓ `({"x"})` | ✓ |
| Duck-typed args (any struct with matching fields) | — | ✓ |
| `consteval` field validation | — | ✓ compile-time enum checking |

On C++20, enum fields like `mode` are validated at compile time — passing an
invalid string is a compile error. On C++17, the same API compiles and runs
but invalid values are only caught at runtime by the Notecard.


### Setting the C++ standard

#### PlatformIO (Arduino framework)

```ini
; platformio.ini
[env:myboard]
build_flags = -std=gnu++20    ; or gnu++23
```

Common platform defaults:
- **ESP32 (pioarduino)**: defaults to gnu++11. Set `-std=gnu++23` for full C++20 features.
- **nRF52/nRF53 (Arduino)**: defaults to gnu++11. Set `-std=gnu++17` or higher.
- **STM32 (STM32duino)**: defaults to gnu++14. Set `-std=gnu++17` or higher.

#### PlatformIO (ESP-IDF framework)

```ini
; platformio.ini — ESP-IDF uses CMake, not build_flags for C++ standard
build_flags = -std=gnu++20
```

Or in your component's `CMakeLists.txt`:
```cmake
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
```

#### Zephyr

In `prj.conf` or your board's config:
```
CONFIG_STD_CPP20=y
```

Or in `CMakeLists.txt`:
```cmake
set(CMAKE_CXX_STANDARD 20)
```


## IDE discoverability

The API is designed for autocomplete-driven discovery:

1. **Type `api.`** — groups appear: `card`, `hub`, `note`, `env`, `file`, etc.
2. **Type `api.card.`** — endpoints appear: `version()`, `temp()`, `binary`, etc.
3. **Type `api.card.temp().`** — operations appear: `read()`, `configure()`, `stop()`
4. **After an operation, type `.`** — fields and `execute()` appear

For aliases with positional args, the IDE shows parameter names and types
in the signature tooltip:

```
remove(string_view file_arg, string_view noteId_arg) → NoteDelete
remove(RemoveArgs args) → NoteDelete
remove() → NoteDelete
```

The `Args` struct definition is visible in the tooltip, showing which fields
are available for designated init.
