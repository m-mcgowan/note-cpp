// Stack-type comparison sample: a single application picks ONE stack
// configuration at compile time (HardwareSerial = A, header-only mock
// = B) and exercises that stack's workload.
//
// On AVR this is the realistic shape — a 2 KB-RAM MCU never carries two
// arenas or two Notecard instances. The selector lets you swap which
// stack the same application code targets; the resulting flash + RAM
// numbers show what each stack configuration actually costs.
//
// Setting NOTECPP_MULTISTACK_USE=AB instantiates both stacks for
// host-side codegen-sharing measurements. AB does not fit AVR.

#ifdef USE_NOTECPP_MULTISTACK

// Serial-only AVR app — see main_avr_notecpp.cpp for the rationale.
#define NOTE_ARDUINO_NO_WIRE
#include <note.hpp>

// Workload selector — choose at compile time which stack the workload
// exercises and which Notecard instances are linked into the build.
// Each AVR env picks exactly one (A or B). Running both (AB) is for
// host-side codegen-sharing measurements only — it doubles the arena
// RAM and overflows AVR flash.
//
//   -DNOTECPP_MULTISTACK_USE=A   real HardwareSerial stack
//   -DNOTECPP_MULTISTACK_USE=B   header-only mock stack
//   -DNOTECPP_MULTISTACK_USE=AB  both (host-only — doesn't fit AVR)
#ifndef NOTECPP_MULTISTACK_USE
#define NOTECPP_MULTISTACK_USE A
#endif

#define NOTECPP_MS_A 1
#define NOTECPP_MS_B 2
#define NOTECPP_MS_AB 3
#define NOTECPP_MS_CONCAT_(x) NOTECPP_MS_##x
#define NOTECPP_MS_CONCAT(x) NOTECPP_MS_CONCAT_(x)
#define NOTECPP_MULTISTACK_USE_INT NOTECPP_MS_CONCAT(NOTECPP_MULTISTACK_USE)

#define NOTECPP_USE_A \
    (NOTECPP_MULTISTACK_USE_INT == NOTECPP_MS_A || NOTECPP_MULTISTACK_USE_INT == NOTECPP_MS_AB)
#define NOTECPP_USE_B \
    (NOTECPP_MULTISTACK_USE_INT == NOTECPP_MS_B || NOTECPP_MULTISTACK_USE_INT == NOTECPP_MS_AB)

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

// ─── Stack A: real HardwareSerial (the production path) ───────────
#if NOTECPP_USE_A
alignas(4) static char arena_a[kArenaSize];
static note::MonotonicArena arena_a_obj(arena_a);

using StackA = note::arduino::SerialTransportStack<HardwareSerial>;
using NotecardA = note::StaticNotecard<StackA>;
static NotecardA nc_a(note::arena_allocator(arena_a_obj), Serial, 9600);
static note::Api<NotecardA> api_a(nc_a);
#endif

// ─── Stack B: header-only mock serial ─────────────────────────────
// Distinct type → distinct StaticNotecard<...> → distinct Api<...>
// instantiation. The mock just discards bytes; the goal of the AB
// variant is codegen-sharing measurement, not a second wire path.
#if NOTECPP_USE_B
struct MockSerial {
    void begin(unsigned long) {}
    size_t write(const uint8_t*, size_t n) { return n; }
    void flush() {}
    int available() { return 0; }
    size_t readBytes(uint8_t*, size_t) { return 0; }
};
static MockSerial mock_serial;

alignas(4) static char arena_b[kArenaSize];
static note::MonotonicArena arena_b_obj(arena_b);

using StackB = note::arduino::SerialTransportStack<MockSerial>;
using NotecardB = note::StaticNotecard<StackB>;
static NotecardB nc_b(note::arena_allocator(arena_b_obj), mock_serial, 9600);
static note::Api<NotecardB> api_b(nc_b);
#endif

// ─── Workload — identical body across both stacks. The whole point
// of the comparison is that the application code is unchanged; only
// the underlying stack type differs.
#if NOTECPP_USE_A
static void workload_a() {
    api_a.hub.set().product("com.example.app").mode("periodic").execute();
    api_a.note.templates().define("sensors.qo")
        .body(note::body([](note::JsonBuilder& b) {
            b.add("temperature", 14.1);
            b.add("humidity", 1);
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
#endif

#if NOTECPP_USE_B
static void workload_b() {
    api_b.hub.set().product("com.example.app").mode("periodic").execute();
    api_b.note.templates().define("sensors.qo")
        .body(note::body([](note::JsonBuilder& b) {
            b.add("temperature", 14.1);
            b.add("humidity", 1);
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
#endif

void setup() {
#if NOTECPP_USE_A
    workload_a();
#endif
#if NOTECPP_USE_B
    workload_b();
#endif
}

void loop() {}

#endif // USE_NOTECPP_MULTISTACK
