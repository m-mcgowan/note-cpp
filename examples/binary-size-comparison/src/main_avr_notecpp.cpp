// Binary size comparison: note-cpp on AVR.
//
// Realistic 8-endpoint app: configure, template, read sensors, publish,
// check status, read voltage, read inbound notes, get environment vars.
//
// API_STYLE selects the developer experience vs code size trade-off:
//   1 = Api groups:   api.hub.set().product(...).execute()
//   2 = Direct:       nc.execute(req)

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

using UsedRequests = note::RequestSet<
    note::api::HubSet,
    note::api::NoteTemplate::Define,
    note::api::CardTemp::Read,
    note::api::NoteAdd,
    note::api::CardStatus,
    note::api::CardVoltage::Read,
    note::api::NoteGet::Read,
    note::api::EnvGet
>;
static constexpr size_t kArenaSize = UsedRequests::max_arena_size;
static_assert(kArenaSize > 0, "arena size must be non-zero");

alignas(4) static char arena_buf[kArenaSize];
static note::MonotonicArena arena(arena_buf);

using SerialNotecard = note::StaticNotecard<note::arduino::SerialTransportStack<>>;
static SerialNotecard nc(note::arena_allocator(arena), Serial, 9600);

#ifndef API_STYLE
#define API_STYLE 1
#endif

#if API_STYLE == 1
static note::Api<SerialNotecard> api(nc);
#endif

void setup() {
#if API_STYLE == 1
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
#else
    {
        note::api::HubSet req;
        req.product = "com.example.size-test";
        req.mode = "periodic";
        req.outbound = 60;
        nc.execute(req);
    }
    {
        note::api::NoteTemplate::Define req;
        req.file = "sensors.qo";
        req.body(note::body([](note::JsonBuilder& b) {
            b.add("temperature", 14.1);
            b.add("humidity", int32_t{1});
        }));
        nc.execute(req);
    }
#endif
}

void loop() {
    arena.reset();

    // Read temperature
#if API_STYLE == 1
    auto temp = api.card.temp().read().execute();
#else
    auto temp = nc.execute(note::api::CardTemp::Read{});
#endif
    float temperature = temp ? temp.value : 0;

    // Publish sensor data
    Readings out{temperature, 60};
#if API_STYLE == 1
    api.note.add()
        .file("sensors.qo")
        .body(out)
        .execute();
#else
    {
        note::api::NoteAdd req;
        req.file = "sensors.qo";
        req.body(out);
        nc.execute(req);
    }
#endif

    // Check connection status
#if API_STYLE == 1
    auto status = api.card.status().execute();
#else
    auto status = nc.execute(note::api::CardStatus{});
#endif
    bool connected = status && status.connected;

    // Read battery voltage
#if API_STYLE == 1
    auto volt = api.card.voltage().read().execute();
#else
    auto volt = nc.execute(note::api::CardVoltage::Read{});
#endif
    double voltage = volt ? volt.value : 0;

    // Read inbound note (if any)
#if API_STYLE == 1
    auto note_in = api.note.read("config.qi").execute();
#else
    {
        note::api::NoteGet::Read req;
        req.file = "config.qi";
        auto note_in = nc.execute(req);
        (void)note_in;
    }
#endif

    // Read environment variable
#if API_STYLE == 1
    auto env = api.env.get().execute();
#else
    auto env = nc.execute(note::api::EnvGet{});
#endif

    (void)connected;
    (void)voltage;
    (void)env;

    delay(60000);
}

#endif
