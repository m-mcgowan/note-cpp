// Binary size comparison: note-cpp on AVR.
//
// Wokwi testing on Uno: Serial for Notecard (shared with serial monitor).
// Debug output comes from the mock chip's printf, not from the firmware.

#ifdef USE_NOTECPP_AVR

#include <note/static_notecard.hpp>
#include <note/api.hpp>
#include <note/request_set.hpp>
#include <note/arduino/begin.hpp>

struct Readings {
    float temperature;
    int32_t humidity;
    NOTE_FIELDS(temperature, humidity)
};

// Compute arena size from the endpoints actually used.
using UsedRequests = note::RequestSet<
    note::api::HubSet,
    note::api::NoteTemplate::Define,
    note::api::CardTemp::Read,
    note::api::NoteAdd
>;
static constexpr size_t kArenaSize = UsedRequests::max_arena_size;
static_assert(kArenaSize > 0, "arena size must be non-zero");

alignas(4) static char arena_buf[kArenaSize];
static note::MonotonicArena arena(arena_buf);

using SerialNotecard = note::StaticNotecard<note::arduino::SerialTransportStack<>>;
static SerialNotecard nc(note::arena_allocator(arena), Serial, 9600);
static note::Api<SerialNotecard> api(nc);

void setup() {
    api.hub.set()
        .product("com.example.size-test")
        .mode("periodic")
        .outbound(60)
        .execute();

    api.note.templates().define("sensors.qo")
        .body(note::body([](note::JsonBuilder& b) {
            b.add("temperature", 14.1);
            b.add("humidity", int32_t{1});
        }))
        .execute();
}

void loop() {
    arena.reset();

    auto temp = api.card.temp().read().execute();
    float temperature = temp ? temp.value : 0;

    Readings out{temperature, 60};
    api.note.add()
        .file("sensors.qo")
        .body(out)
        .execute();

    delay(60000);
}

#endif
