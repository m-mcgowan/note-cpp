# API Calling Patterns

Every Notecard API endpoint in `note-cpp` supports multiple calling styles.
A simple one-liner compiles to the same code as the verbose builder form.

## Quick reference

```cpp
note::Api api(nc);

// ── one-liner with positional args ─────────────────────────
api.hub.set().mode("periodic").execute();
api.note.read("data.qi").execute();
api.note.remove("data.db", "my-note").execute();
api.file.remove("old-data.db").execute();

// ── Designated initializers (C++20) ─────────────────────────────────────
api.note.read({.file = "data.qi"}).execute();
api.note.remove({.file = "data.db", .noteId = "my-note"}).execute();
api.file.remove({.files = {"a.db", "b.db"}}).execute();

// ── Builder pattern ─────────────────────────────────────────────────────
auto req = api.hub.set();
req.mode("periodic");
req.product("com.example.app");
req.execute();
```

## The five calling forms

### 1. Fluent builder (works everywhere)

Every endpoint returns a builder with typed setters that chain:

```cpp
api.hub.set()
    .mode("periodic")
    .product("com.example.app")
    .outbound(int32_t{60})
    .execute();
```

Each setter returns a reference to the builder, so calls chain naturally.
`execute()` sends the request and returns a typed `ApiResult<Response>`.

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

## Array fields

Some request fields accept multiple values (e.g. `file.delete` takes a list
of filenames). These use `ArrayField<T, N>` which supports several
initialization styles:

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

Single-value assignment (`req.files = "data.qi"`) uses standard `operator=`
semantics — it replaces the array contents. Use `add()` to append.

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

On error, `rsp` is falsy and `rsp.error()` returns the `ErrorInfo`:

```cpp
if (!rsp) {
    auto err = rsp.error();
    // err.code: Error::Notecard, Error::SendFailed, etc.
    // err.message: string_view with Notecard's error text
}
```

## C++17 vs C++20

The library targets C++17. C++20 adds ergonomic improvements but is not
required.

| Feature | C++17 | C++20 |
|---------|-------|-------|
| Fluent builder | ✓ | ✓ |
| Positional shorthand | ✓ | ✓ |
| Designated initializers | — | ✓ `{.file = "x"}` |
| Args struct (nested braces) | ✓ `({"x"})` | ✓ |
| Duck-typed args (any struct with matching fields) | — | ✓ |
| `constexpr` field validation | — | ✓ `validatedMode("periodic")` |
| `note::span` aliases `std::span` | — | ✓ (C++17 uses custom impl) |

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

#### Arduino IDE

Arduino IDE does not expose C++ standard settings directly. Use PlatformIO
for C++17+ features, or add compiler flags via `platform.local.txt`:

```
# ~/.arduino15/packages/<platform>/hardware/<arch>/<version>/platform.local.txt
compiler.cpp.extra_flags=-std=gnu++17
```

## IDE discoverability

The API is designed for autocomplete-driven discovery:

1. **Type `api.`** — groups appear: `card`, `hub`, `note`, `env`, `file`, etc.
2. **Type `api.card.`** — endpoints appear: `version()`, `temp()`, `binary`, etc.
3. **Type `api.card.temp().`** — intents appear: `read()`, `configure()`, `stop()`
4. **After an intent, type `.`** — fields and `execute()` appear

For aliases with positional args, the IDE shows parameter names and types
in the signature tooltip:

```
remove(string_view file_arg, string_view noteId_arg) → NoteDelete
remove(RemoveArgs args) → NoteDelete
remove() → NoteDelete
```

The `Args` struct definition is visible in the tooltip, showing which fields
are available for designated init.
