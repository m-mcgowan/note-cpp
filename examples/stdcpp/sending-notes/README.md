# Sending Notes

Every way to send data to (and receive data from) the Notecard using note-cpp.

> All example code on this page comes from [`main.cpp`](main.cpp), which is
> compiled as part of CI to verify correctness.

## Body struct

Define a struct once — use it to send data, receive data, and register
Notecard templates. On C++20, plain aggregates work automatically. On C++17,
add the `NOTE_FIELDS` macro.

<!-- snippet:body-struct examples/stdcpp/sending-notes/main.cpp:41-45 -->
```cpp
struct Readings {
    float temperature;
    int16_t humidity;
    NOTE_FIELDS(temperature, humidity)
};
```

## 1. Ad-hoc note.add

The simplest approach — build JSON fields with a lambda. No generated types
needed, just `notecard.hpp`.

<!-- snippet:adhoc examples/stdcpp/sending-notes/main.cpp:67-70 -->
```cpp
nc.request("note.add", [](note::JsonBuilder& b) {
    b.add("file", "sensors.qo");
    b.add("body", R"({"temp":22.5,"humidity":60})");
});
```

## 2. Builder body

Use `note::body()` to construct the body with a lambda. The typed API handles
the `"req"` and `"file"` fields; you just provide the body content.

<!-- snippet:builder-body examples/stdcpp/sending-notes/main.cpp:80-86 -->
```cpp
api.note.add()
    .file("sensors.qo")
    .body(note::body([](note::JsonBuilder& b) {
        b.add("temp", 22.5);
        b.add("humidity", int32_t{60});
    }))
    .execute();
```

## 3. Typed body struct (recommended)

Pass the struct directly. Field names are extracted automatically via C++20
reflection.

<!-- snippet:typed-body examples/stdcpp/sending-notes/main.cpp:102-103 -->
```cpp
Readings r{.temperature = 22.5f, .humidity = 60};
api.note.add().file("sensors.qo").body(r).execute();
```

## 4. Template registration

Register a Notecard template to enable compact storage. `template_of(Readings())`
auto-generates the type hints (`14.1` = TFLOAT32, `11` = TINT16).

<!-- snippet:template-register examples/stdcpp/sending-notes/main.cpp:114-116 -->
```cpp
api.note.templates().define("sensors.qo")
    .body(note::template_of(Readings()))
    .execute();
```

## 5. Template + send (the production pattern)

Register the template once at startup, then send notes. The Notecard stores
them at a fraction of the size.

<!-- snippet:template-send examples/stdcpp/sending-notes/main.cpp:133-143 -->
```cpp
// Register the template once at startup. template_of(Readings())
// generates type hints from your struct's field types:
//   float    → 14.1 (TFLOAT32)
//   int16_t  → 11   (TINT16)
api.note.templates().define("sensors.qo")
    .body(note::template_of(Readings()))
    .execute();

// Then send notes as usual — the Notecard stores them compactly.
Readings r{.temperature = 22.5f, .humidity = 60};
api.note.add().file("sensors.qo").body(r).execute();
```

## 6. Receive and parse

Read a note and parse the body directly into your struct with `.into(data)`.

<!-- snippet:receive examples/stdcpp/sending-notes/main.cpp:155-160 -->
```cpp
Readings data{};
auto result = api.note.read("data.qi").into(data).execute();
if (result) {
    (void)data.temperature;
    (void)data.humidity;
}
```

## 7. Fire-and-forget command

Send `"cmd"` instead of `"req"` — the Notecard won't send a response, saving
a round-trip.

<!-- snippet:command examples/stdcpp/sending-notes/main.cpp:172-173 -->
```cpp
Readings r{.temperature = 22.5f, .humidity = 60};
api.note.add().file("sensors.qo").body(r).command();
```

## 8. Compile-time JSON

For static requests that never change, `JsonBuf` builds the JSON at compile
time. Zero allocation, zero runtime cost. The compiler verifies the JSON is
well-formed.

<!-- snippet:constexpr-json examples/stdcpp/sending-notes/main.cpp:185-196 -->
```cpp
constexpr auto json = note::json<[](auto& b) {
    b.add("req", "note.add");
    b.add("file", "sensors.qo");
    b.begin_object("body");
        b.add("temp", 22.5);
        b.add("humidity", 60);
    b.end_object();
    b.close();
}>();

static_assert(json.view() ==
    R"({"req":"note.add","file":"sensors.qo","body":{"temp":22.5,"humidity":60}})");
```
