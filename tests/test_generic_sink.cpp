// Tests for GenericResponseSink and GenericBodySink uncovered branches.
//
// GenericResponseSink: body_depth_ forwarding, body detection, reset with all
// field types, array/object forwarding in body.
//
// GenericBodySink: all field type paths (Int8, Int16, Float32, Double),
// on_number dispatch, assign_numeric branches, reset all types.

#include <doctest.h>
#include <note/generic_sink.hpp>
#include <note/struct_sink.hpp>
#include <note/arena.hpp>
#include <note/allocator.hpp>
#include <note/string_pool.hpp>
#include <note/field.hpp>

#include <cstring>
#include <vector>

// ═══════════════════════════════════════════════════════════════════════
// Test response struct with all response field types
// ═══════════════════════════════════════════════════════════════════════

namespace {

struct AllTypesRsp {
    note::ResponseField<bool> flag;
    note::ResponseField<int32_t> count;
    note::ResponseField<double> value;
    note::ResponseField<note::string_view> name;
};

// Hand-written field descriptor table for the response struct.
// GenericResponseSink uses only Bool/Int32/Double/String for response fields.
// ResponseField<T> is non-standard-layout, so we suppress the offsetof warning
// (same approach as codegen uses in generated api headers).
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
static const note::FieldDesc all_types_rsp_fields[] = {
    {"flag",  static_cast<uint16_t>(offsetof(AllTypesRsp, flag)),  note::FieldType::Bool},
    {"count", static_cast<uint16_t>(offsetof(AllTypesRsp, count)), note::FieldType::Int},
    {"value", static_cast<uint16_t>(offsetof(AllTypesRsp, value)), note::FieldType::Double},
    {"name",  static_cast<uint16_t>(offsetof(AllTypesRsp, name)),  note::FieldType::String},
};
#pragma GCC diagnostic pop
constexpr uint8_t all_types_rsp_n = 4;

// Response struct with narrow types to test Int8/Int16/Float32 in reset().
struct NarrowRsp {
    note::ResponseField<bool> flag;
    note::ResponseField<int32_t> count;
    note::ResponseField<double> value;
    note::ResponseField<note::string_view> name;
};

// A FieldDesc table with Int8, Int16, Float32 types — these are used in
// reset() switch branches that are otherwise uncovered.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
static const note::FieldDesc narrow_rsp_fields[] = {
    {"i8",  static_cast<uint16_t>(offsetof(NarrowRsp, flag)),  note::FieldType::Int8},
    {"i16", static_cast<uint16_t>(offsetof(NarrowRsp, count)), note::FieldType::Int16},
    {"f32", static_cast<uint16_t>(offsetof(NarrowRsp, value)), note::FieldType::Float32},
    {"str", static_cast<uint16_t>(offsetof(NarrowRsp, name)),  note::FieldType::String},
};
#pragma GCC diagnostic pop
constexpr uint8_t narrow_rsp_n = 4;

// ── Recording BodyHandler for verifying forwarded events ─────────────

struct RecordedEvent {
    note::BodyEvent::Tag tag;
    std::string key;
    // Value storage:
    bool b = false;
    note::json_int_t i = 0;
    double f = 0.0;
    std::string sv;
};

std::vector<RecordedEvent> g_recorded_events;

void recording_dispatch(void*, const note::BodyEvent& ev) {
    RecordedEvent rec;
    rec.tag = ev.tag;
    rec.key = std::string(ev.key.data(), ev.key.size());
    switch (ev.tag) {
    case note::BodyEvent::Bool:   rec.b = ev.b; break;
    case note::BodyEvent::Int:    rec.i = ev.i; break;
    case note::BodyEvent::Float:  rec.f = ev.f; break;
    case note::BodyEvent::String: rec.sv = std::string(ev.sv.data, ev.sv.len); break;
    case note::BodyEvent::Number: rec.sv = std::string(ev.sv.data, ev.sv.len); break;
    default: break;
    }
    g_recorded_events.push_back(rec);
}

// Use a non-null sentinel as ctx — BodyHandler::operator bool() checks ctx != nullptr.
static int g_recording_sentinel = 0;

note::BodyHandler make_recording_handler() {
    g_recorded_events.clear();
    return {&g_recording_sentinel, recording_dispatch};
}

} // namespace


// ═══════════════════════════════════════════════════════════════════════
// GenericResponseSink tests
// ═══════════════════════════════════════════════════════════════════════

TEST_CASE("GenericResponseSink: basic field assignment") {
    AllTypesRsp rsp{};
    char buf[512];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));

    note::GenericResponseSink sink{&rsp, all_types_rsp_fields, all_types_rsp_n, &pool};

    sink.on_bool("flag", true);
    sink.on_int("count", 42);
    sink.on_float("value", 3.14);
    sink.on_string("name", "hello");

    REQUIRE(rsp.flag.value() == true);
    REQUIRE(rsp.count.value() == 42);
    REQUIRE(rsp.value.value() == doctest::Approx(3.14));
    REQUIRE(rsp.name.value() == "hello");
}

TEST_CASE("GenericResponseSink: body_depth forwarding for on_bool") {
    AllTypesRsp rsp{};
    char buf[256];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));

    note::GenericResponseSink sink{&rsp, all_types_rsp_fields, all_types_rsp_n, &pool};
    sink.set_body_handler(make_recording_handler());

    // Enter body
    sink.on_object_begin("body");
    REQUIRE(sink.body_depth_ == 1);

    // Send events while in body — should forward to handler
    sink.on_bool("active", true);
    REQUIRE(g_recorded_events.size() == 1);
    REQUIRE(g_recorded_events[0].tag == note::BodyEvent::Bool);
    REQUIRE(g_recorded_events[0].key == "active");
    REQUIRE(g_recorded_events[0].b == true);

    // Should NOT assign to response field
    REQUIRE(rsp.flag.has_value() == false);
}

TEST_CASE("GenericResponseSink: body_depth forwarding for on_int") {
    AllTypesRsp rsp{};
    char buf[256];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));

    note::GenericResponseSink sink{&rsp, all_types_rsp_fields, all_types_rsp_n, &pool};
    sink.set_body_handler(make_recording_handler());

    sink.on_object_begin("body");
    sink.on_int("count", 99);

    REQUIRE(g_recorded_events.size() == 1);
    REQUIRE(g_recorded_events[0].tag == note::BodyEvent::Int);
    REQUIRE(g_recorded_events[0].i == 99);
    REQUIRE(rsp.count.has_value() == false);
}

TEST_CASE("GenericResponseSink: body_depth forwarding for on_float") {
    AllTypesRsp rsp{};
    char buf[256];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));

    note::GenericResponseSink sink{&rsp, all_types_rsp_fields, all_types_rsp_n, &pool};
    sink.set_body_handler(make_recording_handler());

    sink.on_object_begin("body");
    sink.on_float("temp", 22.5);

    REQUIRE(g_recorded_events.size() == 1);
    REQUIRE(g_recorded_events[0].tag == note::BodyEvent::Float);
    REQUIRE(g_recorded_events[0].f == doctest::Approx(22.5));
}

TEST_CASE("GenericResponseSink: body_depth forwarding for on_string") {
    AllTypesRsp rsp{};
    char buf[256];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));

    note::GenericResponseSink sink{&rsp, all_types_rsp_fields, all_types_rsp_n, &pool};
    sink.set_body_handler(make_recording_handler());

    sink.on_object_begin("body");
    sink.on_string("label", "sensor-1");

    REQUIRE(g_recorded_events.size() == 1);
    REQUIRE(g_recorded_events[0].tag == note::BodyEvent::String);
    REQUIRE(g_recorded_events[0].sv == "sensor-1");
}

TEST_CASE("GenericResponseSink: body_depth forwarding for on_number") {
    AllTypesRsp rsp{};
    char buf[256];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));

    note::GenericResponseSink sink{&rsp, all_types_rsp_fields, all_types_rsp_n, &pool};
    sink.set_body_handler(make_recording_handler());

    sink.on_object_begin("body");
    sink.on_number("raw", "42.5");

    REQUIRE(g_recorded_events.size() == 1);
    REQUIRE(g_recorded_events[0].tag == note::BodyEvent::Number);
    REQUIRE(g_recorded_events[0].sv == "42.5");
}

TEST_CASE("GenericResponseSink: on_null in body forwards to handler") {
    AllTypesRsp rsp{};
    char buf[256];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));

    note::GenericResponseSink sink{&rsp, all_types_rsp_fields, all_types_rsp_n, &pool};
    sink.set_body_handler(make_recording_handler());

    sink.on_object_begin("body");
    sink.on_null("nothing");

    REQUIRE(g_recorded_events.size() == 1);
    REQUIRE(g_recorded_events[0].tag == note::BodyEvent::Bool);
    REQUIRE(g_recorded_events[0].b == false);
}

TEST_CASE("GenericResponseSink: nested object in body increments depth") {
    AllTypesRsp rsp{};
    char buf[256];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));

    note::GenericResponseSink sink{&rsp, all_types_rsp_fields, all_types_rsp_n, &pool};
    sink.set_body_handler(make_recording_handler());

    // Enter body
    sink.on_object_begin("body");
    REQUIRE(sink.body_depth_ == 1);

    // Nested object inside body
    sink.on_object_begin("nested");
    REQUIRE(sink.body_depth_ == 2);
    REQUIRE(g_recorded_events.size() == 1);
    REQUIRE(g_recorded_events[0].tag == note::BodyEvent::ObjectBegin);

    // Value inside nested object
    sink.on_int("x", 42);
    REQUIRE(g_recorded_events.size() == 2);

    // End nested object — depth stays > 0, so ObjectEnd is forwarded
    sink.on_object_end("nested");
    REQUIRE(sink.body_depth_ == 1);
    REQUIRE(g_recorded_events.size() == 3);
    REQUIRE(g_recorded_events[2].tag == note::BodyEvent::ObjectEnd);

    // End body — depth goes to 0, no event forwarded
    sink.on_object_end("body");
    REQUIRE(sink.body_depth_ == 0);
    REQUIRE(g_recorded_events.size() == 3); // no new event
}

TEST_CASE("GenericResponseSink: array_begin/end in body forward to handler") {
    AllTypesRsp rsp{};
    char buf[256];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));

    note::GenericResponseSink sink{&rsp, all_types_rsp_fields, all_types_rsp_n, &pool};
    sink.set_body_handler(make_recording_handler());

    sink.on_object_begin("body");

    sink.on_array_begin("items");
    REQUIRE(g_recorded_events.size() == 1);
    REQUIRE(g_recorded_events[0].tag == note::BodyEvent::ArrayBegin);

    sink.on_int("", 1);
    sink.on_int("", 2);

    sink.on_array_end("items");
    REQUIRE(g_recorded_events.back().tag == note::BodyEvent::ArrayEnd);
}

TEST_CASE("GenericResponseSink: body events without handler are safe") {
    AllTypesRsp rsp{};
    char buf[256];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));

    // No body handler set
    note::GenericResponseSink sink{&rsp, all_types_rsp_fields, all_types_rsp_n, &pool};

    sink.on_object_begin("body");
    REQUIRE(sink.body_depth_ == 1);

    // These should not crash even without a handler
    sink.on_bool("x", true);
    sink.on_int("x", 1);
    sink.on_float("x", 1.0);
    sink.on_string("x", "y");
    sink.on_number("x", "1");
    sink.on_null("x");
    sink.on_object_begin("nested");
    sink.on_object_end("nested");
    sink.on_array_begin("arr");
    sink.on_array_end("arr");
    sink.on_object_end("body");

    REQUIRE(sink.body_depth_ == 0);
}

TEST_CASE("GenericResponseSink: non-body object_begin is ignored") {
    AllTypesRsp rsp{};
    char buf[256];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));

    note::GenericResponseSink sink{&rsp, all_types_rsp_fields, all_types_rsp_n, &pool};

    // An object that is NOT "body" should not set body_depth
    sink.on_object_begin("other");
    REQUIRE(sink.body_depth_ == 0);
}

TEST_CASE("GenericResponseSink: reset with body handler") {
    AllTypesRsp rsp{};
    char buf[512];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));

    note::GenericResponseSink sink{&rsp, all_types_rsp_fields, all_types_rsp_n, &pool};
    sink.set_body_handler(make_recording_handler());

    // Set some values
    sink.on_bool("flag", true);
    sink.on_int("count", 42);
    sink.on_float("value", 3.14);
    sink.on_string("name", "test");

    // Also enter and exit body to set body_depth
    sink.on_object_begin("body");
    sink.body_depth_ = 3; // simulate nested body

    sink.reset();

    REQUIRE(sink.body_depth_ == 0);
    REQUIRE(rsp.flag.value() == false);
    REQUIRE(rsp.count.value() == 0);
    REQUIRE(rsp.value.value() == 0.0);
    REQUIRE(rsp.name.value().empty());

    // Should have sent Reset event to handler
    REQUIRE(g_recorded_events.back().tag == note::BodyEvent::Reset);
}

TEST_CASE("GenericResponseSink: reset exercises Int8/Int16/Float32 branches") {
    // Use a field table that maps to Int8/Int16/Float32 types to exercise
    // those branches in the reset switch.
    NarrowRsp rsp{};
    char buf[256];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));

    note::GenericResponseSink sink{&rsp, narrow_rsp_fields, narrow_rsp_n, &pool};

    // Write some data to the raw bytes
    rsp.flag.value_ = true;
    rsp.flag.present_ = true;

    sink.reset();

    // After reset, the fields should be back to default (the reset_field
    // template writes a default-constructed ResponseField<T>).
    REQUIRE(rsp.flag.present_ == false);
}

TEST_CASE("GenericResponseSink: reset without body handler") {
    AllTypesRsp rsp{};
    char buf[256];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));

    note::GenericResponseSink sink{&rsp, all_types_rsp_fields, all_types_rsp_n, &pool};
    // No body handler set

    sink.on_bool("flag", true);
    sink.reset();

    REQUIRE(rsp.flag.value() == false);
    REQUIRE(sink.body_depth_ == 0);
}


// ═══════════════════════════════════════════════════════════════════════
// GenericBodySink tests — covering all field types including narrow types
// ═══════════════════════════════════════════════════════════════════════

namespace {

// Body struct with ALL field types to exercise every code path.
struct AllFieldBody {
    bool flag;
    int8_t tiny;
    int16_t medium;
    int32_t large;
    float precise;
    double extra_precise;
    note::string_view label;
    NOTE_FIELDS(flag, tiny, medium, large, precise, extra_precise, label)
};

} // namespace

TEST_CASE("GenericBodySink: on_bool assigns bool field") {
    AllFieldBody data{};
    char buf[256];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));

    uint8_t n = 0;
    auto* descs = AllFieldBody::_note_field_descs<AllFieldBody>(n);
    note::GenericBodySink sink{&data, descs, n, &pool};

    sink.on_bool("flag", true);
    REQUIRE(data.flag == true);

    // Non-bool fields should not be assigned by on_bool
    sink.on_bool("tiny", true);
    REQUIRE(data.tiny == 0);
}

TEST_CASE("GenericBodySink: on_int assigns to all integer/float types") {
    AllFieldBody data{};
    char buf[256];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));

    uint8_t n = 0;
    auto* descs = AllFieldBody::_note_field_descs<AllFieldBody>(n);
    note::GenericBodySink sink{&data, descs, n, &pool};

    sink.on_int("tiny", 42);
    sink.on_int("medium", 1000);
    sink.on_int("large", 100000);
    sink.on_int("precise", 23);       // int -> Float32
    sink.on_int("extra_precise", 99); // int -> Double

    REQUIRE(data.tiny == 42);
    REQUIRE(data.medium == 1000);
    REQUIRE(data.large == 100000);
    REQUIRE(data.precise == 23.0f);
    REQUIRE(data.extra_precise == 99.0);
}

TEST_CASE("GenericBodySink: on_float assigns to all numeric types") {
    AllFieldBody data{};
    char buf[256];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));

    uint8_t n = 0;
    auto* descs = AllFieldBody::_note_field_descs<AllFieldBody>(n);
    note::GenericBodySink sink{&data, descs, n, &pool};

    sink.on_float("tiny", 7.9);
    sink.on_float("medium", 500.7);
    sink.on_float("large", 99999.1);
    sink.on_float("precise", 22.5);
    sink.on_float("extra_precise", 3.14159);

    REQUIRE(data.tiny == 7);
    REQUIRE(data.medium == 500);
    REQUIRE(data.large == 99999);
    REQUIRE(data.precise == 22.5f);
    REQUIRE(data.extra_precise == doctest::Approx(3.14159));
}

TEST_CASE("GenericBodySink: on_string assigns string field only") {
    AllFieldBody data{};
    char buf[256];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));

    uint8_t n = 0;
    auto* descs = AllFieldBody::_note_field_descs<AllFieldBody>(n);
    note::GenericBodySink sink{&data, descs, n, &pool};

    sink.on_string("label", "hello");
    REQUIRE(data.label == "hello");

    // Non-string fields should not be affected
    sink.on_string("tiny", "nope");
    REQUIRE(data.tiny == 0);
}

TEST_CASE("GenericBodySink: on_number Float32/Double uses parse_double") {
    AllFieldBody data{};
    char buf[256];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));

    uint8_t n = 0;
    auto* descs = AllFieldBody::_note_field_descs<AllFieldBody>(n);
    note::GenericBodySink sink{&data, descs, n, &pool};

    sink.on_number("precise", "22.5");
    sink.on_number("extra_precise", "3.14");

    REQUIRE(data.precise == 22.5f);
    REQUIRE(data.extra_precise == doctest::Approx(3.14));
}

TEST_CASE("GenericBodySink: on_number Int8/Int16/Int32 uses parse_int") {
    AllFieldBody data{};
    char buf[256];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));

    uint8_t n = 0;
    auto* descs = AllFieldBody::_note_field_descs<AllFieldBody>(n);
    note::GenericBodySink sink{&data, descs, n, &pool};

    sink.on_number("tiny", "42");
    sink.on_number("medium", "1234");
    sink.on_number("large", "99999");

    REQUIRE(data.tiny == 42);
    REQUIRE(data.medium == 1234);
    REQUIRE(data.large == 99999);
}

TEST_CASE("GenericBodySink: reset zeros all field types") {
    AllFieldBody data{};
    data.flag = true;
    data.tiny = 42;
    data.medium = 1000;
    data.large = 99999;
    data.precise = 22.5f;
    data.extra_precise = 3.14;
    data.label = "hello";

    char buf[256];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));

    uint8_t n = 0;
    auto* descs = AllFieldBody::_note_field_descs<AllFieldBody>(n);
    note::GenericBodySink sink{&data, descs, n, &pool};

    sink.reset();

    REQUIRE(data.flag == false);
    REQUIRE(data.tiny == 0);
    REQUIRE(data.medium == 0);
    REQUIRE(data.large == 0);
    REQUIRE(data.precise == 0.0f);
    REQUIRE(data.extra_precise == 0.0);
    REQUIRE(data.label.empty());
}

TEST_CASE("GenericBodySink: assign_numeric covers all type branches") {
    AllFieldBody data{};
    char buf[256];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));

    uint8_t n = 0;
    auto* descs = AllFieldBody::_note_field_descs<AllFieldBody>(n);
    note::GenericBodySink sink{&data, descs, n, &pool};

    // on_int hits assign_numeric for Int8, Int16, Int32, Float32, Double
    sink.on_int("tiny", 10);
    sink.on_int("medium", 200);
    sink.on_int("large", 3000);
    sink.on_int("precise", 40);
    sink.on_int("extra_precise", 50);

    REQUIRE(data.tiny == 10);
    REQUIRE(data.medium == 200);
    REQUIRE(data.large == 3000);
    REQUIRE(data.precise == 40.0f);
    REQUIRE(data.extra_precise == 50.0);
}

TEST_CASE("GenericBodySink: assign_numeric default (Bool) is no-op") {
    AllFieldBody data{};
    char buf[256];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));

    uint8_t n = 0;
    auto* descs = AllFieldBody::_note_field_descs<AllFieldBody>(n);
    note::GenericBodySink sink{&data, descs, n, &pool};

    // on_int for a Bool field should match key but assign_numeric's default
    // case does nothing (Bool is not handled by assign_numeric).
    sink.on_int("flag", 1);
    REQUIRE(data.flag == false); // unchanged
}


// ═══════════════════════════════════════════════════════════════════════
// GenericResponseSink + StructSink body handler integration
// ═══════════════════════════════════════════════════════════════════════

namespace {

struct BodyData {
    float temperature;
    int32_t humidity;
    note::string_view label;
    NOTE_FIELDS(temperature, humidity, label)
};

} // namespace

TEST_CASE("GenericResponseSink: body forwarded to StructSink via BodyHandler") {
    AllTypesRsp rsp{};
    BodyData body{};
    char buf[1024];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));

    note::StructSink<BodyData> body_sink(body, pool);
    auto handler = note::make_body_handler(body_sink);

    note::GenericResponseSink sink{&rsp, all_types_rsp_fields, all_types_rsp_n, &pool};
    sink.set_body_handler(handler);

    // Response-level fields
    sink.on_bool("flag", true);
    sink.on_int("count", 10);

    // Body
    sink.on_object_begin("body");
    sink.on_float("temperature", 22.5);
    sink.on_int("humidity", 65);
    sink.on_string("label", "room-42");
    sink.on_object_end("body");

    REQUIRE(rsp.flag.value() == true);
    REQUIRE(rsp.count.value() == 10);
    REQUIRE(body.temperature == 22.5f);
    REQUIRE(body.humidity == 65);
    REQUIRE(body.label == "room-42");
}

TEST_CASE("GenericResponseSink: deeply nested body objects forwarded") {
    AllTypesRsp rsp{};
    char buf[256];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));

    note::GenericResponseSink sink{&rsp, all_types_rsp_fields, all_types_rsp_n, &pool};
    sink.set_body_handler(make_recording_handler());

    sink.on_object_begin("body");
    sink.on_object_begin("level1");
    REQUIRE(sink.body_depth_ == 2);
    sink.on_object_begin("level2");
    REQUIRE(sink.body_depth_ == 3);
    sink.on_int("deep_value", 42);

    sink.on_object_end("level2");
    REQUIRE(sink.body_depth_ == 2);
    sink.on_object_end("level1");
    REQUIRE(sink.body_depth_ == 1);
    sink.on_object_end("body");
    REQUIRE(sink.body_depth_ == 0);

    // Should have recorded: ObjectBegin(level1), ObjectBegin(level2),
    // Int(deep_value), ObjectEnd(level2), ObjectEnd(level1)
    // body open/close are NOT forwarded (they're the container markers)
    REQUIRE(g_recorded_events.size() == 5);
}


// ═══════════════════════════════════════════════════════════════════════
// make_body_handler dispatch — cover remaining BodyEvent tags
// ═══════════════════════════════════════════════════════════════════════

namespace {

struct NestedBody {
    float value;
    note::string_view name;
    NOTE_FIELDS(value, name)
};

struct OuterBody {
    NestedBody child;
    int32_t count;
    NOTE_FIELDS(child, count)
};

struct WithArr {
    std::array<int32_t, 3> vals;
    NOTE_FIELDS(vals)
};

} // namespace

TEST_CASE("make_body_handler: ObjectBegin/End forwards to StructSink") {
    OuterBody body{};
    char buf[1024];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));

    note::StructSink<OuterBody> sink(body, pool);
    auto handler = note::make_body_handler(sink);

    handler.send(note::BodyEvent::make_int("count", 5));
    handler.send(note::BodyEvent::make_object_begin("child"));
    handler.send(note::BodyEvent::make_float("value", 1.5));
    handler.send(note::BodyEvent::make_string("name", "inner"));
    handler.send(note::BodyEvent::make_object_end("child"));

    REQUIRE(body.count == 5);
    REQUIRE(body.child.value == 1.5f);
    REQUIRE(body.child.name == "inner");
}

TEST_CASE("make_body_handler: ArrayBegin/End forwards to StructSink") {
    WithArr body{};
    char buf[1024];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));

    note::StructSink<WithArr> sink(body, pool);
    auto handler = note::make_body_handler(sink);

    handler.send(note::BodyEvent::make_array_begin("vals"));
    handler.send(note::BodyEvent::make_int("", 10));
    handler.send(note::BodyEvent::make_int("", 20));
    handler.send(note::BodyEvent::make_int("", 30));
    handler.send(note::BodyEvent::make_array_end("vals"));

    REQUIRE(body.vals[0] == 10);
    REQUIRE(body.vals[1] == 20);
    REQUIRE(body.vals[2] == 30);
}

TEST_CASE("make_body_handler: Number event forwards to StructSink") {
    OuterBody body{};
    char buf[1024];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));

    note::StructSink<OuterBody> sink(body, pool);
    auto handler = note::make_body_handler(sink);

    handler.send(note::BodyEvent::make_number("count", "42"));
    REQUIRE(body.count == 42);
}

TEST_CASE("make_body_handler: Reset event resets StructSink") {
    OuterBody body{};
    body.count = 99;
    body.child.value = 1.5f;
    char buf[1024];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));

    note::StructSink<OuterBody> sink(body, pool);
    auto handler = note::make_body_handler(sink);

    handler.send(note::BodyEvent::make_reset());
    REQUIRE(body.count == 0);
    REQUIRE(body.child.value == 0.0f);
}

TEST_CASE("make_body_handler: String event forwards to StructSink") {
    OuterBody body{};
    char buf[1024];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));

    note::StructSink<OuterBody> sink(body, pool);
    auto handler = note::make_body_handler(sink);

    handler.send(note::BodyEvent::make_object_begin("child"));
    handler.send(note::BodyEvent::make_string("name", "test-name"));
    handler.send(note::BodyEvent::make_object_end("child"));

    REQUIRE(body.child.name == "test-name");
}

TEST_CASE("make_generic_body_handler: Number event dispatches") {
    AllFieldBody data{};
    char buf[256];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));

    uint8_t n = 0;
    auto* descs = AllFieldBody::_note_field_descs<AllFieldBody>(n);
    note::GenericBodySink sink{&data, descs, n, &pool};

    auto handler = note::make_generic_body_handler(sink);

    handler.send(note::BodyEvent::make_number("precise", "22.5"));
    handler.send(note::BodyEvent::make_number("large", "42"));

    REQUIRE(data.precise == 22.5f);
    REQUIRE(data.large == 42);
}
