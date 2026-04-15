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

<!-- snippet:adhoc examples/stdcpp/sending-notes/main.cpp:67-72 -->
```cpp
// nc.request() sends a raw JSON request — you supply the request name
// and a builder lambda that populates the fields.
nc.request("note.add", [](note::JsonBuilder& b) {
    b.add("file", "sensors.qo");                     // target Notefile
    b.add("body", R"({"temp":22.5,"humidity":60})");  // body as raw JSON string
});
```

## 2. Builder body

Use `note::body()` to construct the body with a lambda. The typed API handles
the `"req"` and `"file"` fields; you just provide the body content.

<!-- snippet:builder-body examples/stdcpp/sending-notes/main.cpp:82-90 -->
```cpp
// note::body() wraps a lambda that builds the body JSON field-by-field.
// The lambda receives a JsonBuilder — call b.add(key, value) for each field.
api.note.add()
    .file("sensors.qo")
    .body(note::body([](note::JsonBuilder& b) {
        b.add("temp", 22.5);       // adds "temp":22.5 to the body object
        b.add("humidity", 60);     // adds "humidity":60 to the body object
    }))
    .execute();                          // sends the request to the Notecard
```

## 3. Typed body struct (recommended)

Pass the struct directly. Field names are extracted automatically via C++20
reflection.

<!-- snippet:typed-body examples/stdcpp/sending-notes/main.cpp:106-107 -->
```cpp
Readings r{.temperature = 22.5f, .humidity = 60};
api.note.add().file("sensors.qo").body(r).execute();
```

## 4. Template registration

Register a Notecard template to enable compact storage. `template_of(Readings())`
auto-generates the type hints (`14.1` = TFLOAT32, `11` = TINT16).

<!-- snippet:template-register examples/stdcpp/sending-notes/main.cpp:118-125 -->
```cpp
// templates().define() tells the Notecard the shape of your data.
// template_of(Readings()) inspects your struct and generates type hints:
//   float   → 14.1 (TFLOAT32)
//   int16_t → 11   (TINT16)
// After this, Notes in "sensors.qo" are stored as compact binary.
api.note.templates().define("sensors.qo")
    .body(note::template_of(Readings()))
    .execute();
```

## 5. Template + send (the production pattern)

Register the template once at startup, then send notes. The Notecard stores
them at a fraction of the size.

<!-- snippet:template-send examples/stdcpp/sending-notes/main.cpp:142-152 -->
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

<!-- snippet:receive examples/stdcpp/sending-notes/main.cpp:164-172 -->
```cpp
Readings data{};  // same struct used for sending — zero boilerplate
// .into(data) tells execute() to parse the Note's body directly into
// the struct. Fields are matched by name — no manual JSON parsing.
auto result = api.note.read("data.qi").into(data).execute();
if (result) {
    // data.temperature and data.humidity are now populated
    (void)data.temperature;
    (void)data.humidity;
}
```

## 7. Fire-and-forget command

Send `"cmd"` instead of `"req"` — the Notecard won't send a response, saving
a round-trip.

<!-- snippet:command examples/stdcpp/sending-notes/main.cpp:184-188 -->
```cpp
// .command() instead of .execute() — sends the request as a "cmd" rather
// than a "req". The Notecard processes it but does NOT send a response,
// so there's nothing to wait for. Good for fire-and-forget telemetry.
Readings r{.temperature = 22.5f, .humidity = 60};
api.note.add().file("sensors.qo").body(r).command();
```

## 8. Compile-time JSON

For static requests that never change, `JsonBuf` builds the JSON at compile
time. Zero allocation, zero runtime cost. The compiler verifies the JSON is
well-formed.

<!-- snippet:constexpr-json examples/stdcpp/sending-notes/main.cpp:200-216 -->
```cpp
// note::json<lambda>() builds JSON entirely at compile time — the result
// is baked into your binary as a string constant. Zero runtime cost.
// The compiler auto-sizes the buffer to fit the output.
constexpr auto json = note::json<[](auto& b) {
    b.add("req", "note.add");        // top-level field: request name
    b.add("file", "sensors.qo");     // top-level field: target Notefile
    b.begin_object("body");          // open a nested JSON object for "body"
        b.add("temp", 22.5);         //   body field
        b.add("humidity", 60);       //   body field
    b.end_object();                  // close the "body" object
    b.close();                       // finalize — no more fields allowed after this
}>();

// Proof this happened at compile time — static_assert only works on
// values known during compilation:
static_assert(json.view() ==
    R"({"req":"note.add","file":"sensors.qo","body":{"temp":22.5,"humidity":60}})");
```
