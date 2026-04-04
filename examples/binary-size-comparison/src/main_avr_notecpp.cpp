// Binary size comparison: note-cpp on AVR.
//
// Full streaming path: request JSON streamed directly to transport via
// StreamingJsonBuilder, response SAX-parsed directly from transport.
// No intermediate buffers, no JsonBackend needed.
//
// Arduino Uno has only one HW serial (Serial), shared with Notecard.
// Same transport as the note-c AVR example for a fair comparison.

#ifdef USE_NOTECPP_AVR

// NOTE_MINIMAL is set via build_flags, before any includes.

#include <note/static_notecard.hpp>
#include <note/api.hpp>
#include <note/arduino/begin.hpp>

// ── Application data struct ────────────────────────────────────────────
// One struct for sending, receiving, and template registration.
struct Readings {
    float temperature;
    int32_t humidity;
    NOTE_FIELDS(temperature, humidity)
};

// Arena for string interning during SAX parse (response string fields).
alignas(4) static char arena_buf[64];
static note::MonotonicArena arena(arena_buf);

// Zero-vtable Notecard: transport stack owned by value, no virtual dispatch.
using SerialNotecard = note::StaticNotecard<note::arduino::SerialTransportStack<>>;
static SerialNotecard notecard(note::arena_allocator(arena), Serial, 9600);
static note::Api<SerialNotecard> nc(notecard);

void setup() {
    Serial.begin(9600);

    // Configure connection
    nc.hub.set()
        .product("com.example.size-test")
        .mode("periodic")
        .outbound(60)
        .execute();

    // Register template (manual type hints — template_of<T>() requires C++20)
    nc.note.templates().define("sensors.qo")
        .body(note::body([](note::JsonBuilder& b) {
            b.add("temperature", 14.1);
            b.add("humidity", int32_t{1});
        }))
        .execute();
}

void loop() {
    arena.reset();  // reclaim arena between requests

    // Read temperature
    auto temp = nc.card.temp().read().execute();
    float temperature = 0;
    if (temp) {
        temperature = temp.value;
    }

    // Publish sensor data with typed body
    Readings out{temperature, 60};
    nc.note.add()
        .file("sensors.qo")
        .body(out)
        .execute();

    // Read back a note with typed body parsing
    Readings in{};
    auto result = nc.note.read("sensors.qo").into(in).execute();
    if (result) {
        (void)in.temperature;
        (void)in.humidity;
    }

    delay(60000);
}

#endif // USE_NOTECPP_AVR
