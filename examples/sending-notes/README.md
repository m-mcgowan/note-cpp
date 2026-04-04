# Sending Notes

Every way to send data to (and receive data from) the Notecard using note-cpp.

> All example code on this page comes from [`main.cpp`](main.cpp), which is
> compiled as part of CI to verify correctness.

## Body struct

Define a struct once — use it to send data, receive data, and register
Notecard templates. On C++20, plain aggregates work automatically. On C++17,
add the `NOTE_FIELDS` macro.

```cpp
// main.cpp#L74-L78

std::puts("\n--- Builder body ---");
api.note.add()
    .file("sensors.qo")
    .body(note::body([](note::JsonBuilder& b) {
        b.add("temp", 22.5);
```

## 1. Ad-hoc note.add

The simplest approach — build JSON fields with a lambda. No generated types
needed, just `notecard.hpp`.

```cpp
// main.cpp#L97-L100

}


// ═════════════════════════════════════════════════════════════════════════
```

## 2. Builder body

Use `note::body()` to construct the body with a lambda. The typed API handles
the `"req"` and `"file"` fields; you just provide the body content.

```cpp
// main.cpp#L108-L114



// ═════════════════════════════════════════════════════════════════════════
// 5. Template + send — the production pattern
//
// Without a template, the Notecard stores each note as a JSON string.
// With a template, it knows the shape of your data upfront and stores
```

## 3. Typed body struct (recommended)

Pass the struct directly. Field names are extracted automatically via C++20
reflection.

```cpp
// main.cpp#L123-L124

// generates type hints from your struct's field types:
//   float    → 14.1 (TFLOAT32)
```

## 4. Template registration

Register a Notecard template to enable compact storage. `template_of<T>()`
auto-generates the type hints (`14.1` = TFLOAT32, `11` = TINT16).

```cpp
// main.cpp#L133-L136

}


// ═════════════════════════════════════════════════════════════════════════
```

## 5. Template + send (the production pattern)

Register the template once at startup, then send notes. The Notecard stores
them at a fraction of the size.

```cpp
// main.cpp#L145-L153

        (void)data.temperature;
        (void)data.humidity;
    }
}


// ═════════════════════════════════════════════════════════════════════════
// 7. Command (fire-and-forget) — sends "cmd" instead of "req"
// ═════════════════════════════════════════════════════════════════════════
```

## 6. Receive and parse

Read a note and parse the body directly into your struct with `.into(data)`.

```cpp
// main.cpp#L163-L168

// 8. Compile-time JSON — zero-allocation constexpr buffer
// ═════════════════════════════════════════════════════════════════════════

std::puts("\n--- Compile-time JSON ---");
{
    constexpr auto json = note::json<[](auto& b) {
```

## 7. Fire-and-forget command

Send `"cmd"` instead of `"req"` — the Notecard won't send a response, saving
a round-trip.

```cpp
// main.cpp#L178-L179

static_assert(json.view() ==
    R"({"req":"note.add","file":"sensors.qo","body":{"temp":22.5,"humidity":60}})");
```

## 8. Compile-time JSON

For static requests that never change, `JsonBuf` builds the JSON at compile
time. Zero allocation, zero runtime cost. The compiler verifies the JSON is
well-formed.

```cpp
// main.cpp#L189-L200


```
