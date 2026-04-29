// Multi-instantiation measurement: two StaticNotecard types in one
// translation unit, sharing as much codegen as possible.
//
// The single-stack sample (main_avr_notecpp.cpp) doesn't exercise
// duplicated codegen — there's only one Notecard type. Several wins
// from the 2026-04-29 size pass were structural (BuildFn dedupe,
// req_wrap_build extraction, dispatch_sax_event outlining) and
// deliver near-zero on the single-stack build but should compound
// when there are two distinct StaticNotecard<...> types in the same
// program: each instantiation previously got its own copy of
// Api::generic_thunk_, send_thunk_, and the per-RequestT closures.
//
// This binary builds two Notecards over different transport stacks
// — HardwareSerial-backed and a header-only mock — and runs the same
// 8-endpoint workload on both. The mock stack keeps the second
// transport's code footprint near zero so the measurement reflects
// thunk/closure duplication, not a second protocol implementation.
//
// Reading the gate: `multistack_flash - single_flash` is the
// duplication cost. If structural wins compound, this delta stays
// well below 1× the single-stack size; a regression here flags lost
// sharing.

#ifdef USE_NOTECPP_MULTISTACK

#include <note/static_notecard.hpp>
#include <note/api.hpp>
#include <note/request_set.hpp>
#include <note/arduino/begin.hpp>
#include <note/json_buf.hpp>

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

// Two arenas, one per Notecard. Static so they don't compete with stack.
alignas(4) static char arena_a[kArenaSize];
alignas(4) static char arena_b[kArenaSize];
static note::MonotonicArena arena_a_obj(arena_a);
static note::MonotonicArena arena_b_obj(arena_b);

// ─── Stack A: real HardwareSerial (the production path) ───────────
using StackA = note::arduino::SerialTransportStack<HardwareSerial>;
using NotecardA = note::StaticNotecard<StackA>;
static NotecardA nc_a(note::arena_allocator(arena_a_obj), Serial, 9600);
static note::Api<NotecardA> api_a(nc_a);

// ─── Stack B: header-only mock serial ─────────────────────────────
// Distinct type → distinct StaticNotecard<...> → distinct Api<...>
// instantiation. The mock just discards bytes; the goal is the
// codegen duplication measurement, not a second wire path.
struct MockSerial {
    void begin(unsigned long) {}
    size_t write(const uint8_t*, size_t n) { return n; }
    void flush() {}
    int available() { return 0; }
    size_t readBytes(uint8_t*, size_t) { return 0; }
};
static MockSerial mock_serial;

using StackB = note::arduino::SerialTransportStack<MockSerial>;
using NotecardB = note::StaticNotecard<StackB>;
static NotecardB nc_b(note::arena_allocator(arena_b_obj), mock_serial, 9600);
static note::Api<NotecardB> api_b(nc_b);

// ─── Workload — identical across both Notecards. The intent is to
// force codegen for every endpoint on both Api types so any thunk
// duplication shows up in the linked binary.
static void workload_a() {
    api_a.hub.set().product("com.example.app").mode("periodic").execute();
    api_a.note.templates().define("sensors.qo")
        .body(note::body([](note::JsonBuilder& b) {
            b.add("temperature", 14.1);
            b.add("humidity", int32_t{1});
        }))
        .execute();
    auto t = api_a.card.temp().read().execute();
    Readings r{.temperature = t ? t.value : 0.0f, .humidity = 60};
    api_a.note.add().file("sensors.qo").body(r).execute();
    api_a.card.status().execute();
    api_a.card.voltage().read().execute();
    api_a.note.get().read().file("inbound.qi").execute();
    api_a.env.get().name("interval").execute();
}

static void workload_b() {
    api_b.hub.set().product("com.example.app").mode("periodic").execute();
    api_b.note.templates().define("sensors.qo")
        .body(note::body([](note::JsonBuilder& b) {
            b.add("temperature", 14.1);
            b.add("humidity", int32_t{1});
        }))
        .execute();
    auto t = api_b.card.temp().read().execute();
    Readings r{.temperature = t ? t.value : 0.0f, .humidity = 60};
    api_b.note.add().file("sensors.qo").body(r).execute();
    api_b.card.status().execute();
    api_b.card.voltage().read().execute();
    api_b.note.get().read().file("inbound.qi").execute();
    api_b.env.get().name("interval").execute();
}

void setup() {
    workload_a();
    workload_b();
}

void loop() {}

#endif // USE_NOTECPP_MULTISTACK
