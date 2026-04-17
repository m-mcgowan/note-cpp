#include "catch.hpp"
#include <note/body.hpp>
#include <note/types.hpp>
#include <type_traits>

namespace {

struct Flat {
    float temperature;
    int32_t humidity;
    bool active;
    NOTE_FIELDS(temperature, humidity, active)
};

} // namespace

TEST_CASE("_note_fields_dispatch matches known fields") {
    Flat obj{};
    bool matched = false;

    matched = Flat::_note_fields_dispatch(obj, "temperature", [](auto& field) {
        using F = std::remove_reference_t<decltype(field)>;
        if constexpr (std::is_floating_point_v<F>) {
            field = 22.5f;
        }
    });
    REQUIRE(matched);
    REQUIRE(obj.temperature == 22.5f);

    matched = Flat::_note_fields_dispatch(obj, "humidity", [](auto& field) {
        using F = std::remove_reference_t<decltype(field)>;
        if constexpr (std::is_integral_v<F> && !std::is_same_v<F, bool>) {
            field = 45;
        }
    });
    REQUIRE(matched);
    REQUIRE(obj.humidity == 45);

    matched = Flat::_note_fields_dispatch(obj, "unknown", [](auto&) {});
    REQUIRE_FALSE(matched);
}

TEST_CASE("_note_fields_dispatch with bool field") {
    Flat obj{};
    Flat::_note_fields_dispatch(obj, "active", [](auto& field) {
        using F = std::remove_reference_t<decltype(field)>;
        if constexpr (std::is_same_v<F, bool>) {
            field = true;
        }
    });
    REQUIRE(obj.active == true);
}

#include <note/struct_sink.hpp>
#include <note/arena.hpp>
#include <note/allocator.hpp>
#include <note/string_pool.hpp>

namespace {

struct SensorData {
    float temperature;
    int32_t humidity;
    bool active;
    NOTE_FIELDS(temperature, humidity, active)
};

struct WithStrings {
    float value;
    note::string_view label;
    note::string_view unit;
    NOTE_FIELDS(value, label, unit)
};

} // namespace

TEST_CASE("StructSink parses flat numeric struct") {
    SensorData data{};
    char buf[256];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));
    note::StructSink<SensorData> sink(data, pool);

    sink.on_float("temperature", 22.5);
    sink.on_int("humidity", 45);
    sink.on_bool("active", true);

    REQUIRE(data.temperature == 22.5f);
    REQUIRE(data.humidity == 45);
    REQUIRE(data.active == true);
}

TEST_CASE("StructSink ignores unknown fields") {
    SensorData data{};
    char buf[256];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));
    note::StructSink<SensorData> sink(data, pool);

    sink.on_float("unknown", 99.9);
    sink.on_string("also_unknown", "hello");

    REQUIRE(data.temperature == 0.0f);
    REQUIRE(data.humidity == 0);
}

TEST_CASE("StructSink parses string fields into arena") {
    WithStrings ws{};
    char buf[512];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));
    note::StructSink<WithStrings> sink(ws, pool);

    sink.on_float("value", 22.5);
    sink.on_string("label", "room-42");
    sink.on_string("unit", "celsius");

    REQUIRE(ws.value == 22.5f);
    REQUIRE(ws.label == "room-42");
    REQUIRE(ws.unit == "celsius");

    // Verify strings are in arena
    REQUIRE(ws.label.data() >= buf);
    REQUIRE(ws.label.data() < buf + sizeof(buf));
}

TEST_CASE("StructSink handles on_number with raw string") {
    SensorData data{};
    char buf[256];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));
    note::StructSink<SensorData> sink(data, pool);

    sink.on_number("temperature", "22.5");
    sink.on_number("humidity", "45");

    REQUIRE(data.temperature == 22.5f);
    REQUIRE(data.humidity == 45);
}

// ── Nested struct + array tests ────────────────────────────────────────

#include <array>

namespace {

struct Location {
    double lat;
    double lon;
    NOTE_FIELDS(lat, lon)
};

struct TripPoint {
    Location location;
    float speed;
    note::string_view label;
    NOTE_FIELDS(location, speed, label)
};

struct WithArrays {
    std::array<float, 4> temps;
    int32_t count;
    NOTE_FIELDS(temps, count)
};

struct WithStructArray {
    std::array<Location, 3> waypoints;
    int32_t count;
    NOTE_FIELDS(waypoints, count)
};

} // namespace

TEST_CASE("StructSink parses nested struct") {
    TripPoint tp{};
    char buf[512];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));
    note::StructSink<TripPoint> sink(tp, pool);

    sink.on_float("speed", 65.5);
    sink.on_string("label", "start");
    sink.on_object_begin("location");
    sink.on_float("lat", 42.3601);
    sink.on_float("lon", -71.0589);
    sink.on_object_end("location");

    REQUIRE(tp.speed == 65.5f);
    REQUIRE(tp.label == "start");
    REQUIRE(tp.location.lat == Approx(42.3601));
    REQUIRE(tp.location.lon == Approx(-71.0589));
}

TEST_CASE("StructSink skips unknown nested objects") {
    TripPoint tp{};
    char buf[256];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));
    note::StructSink<TripPoint> sink(tp, pool);

    sink.on_object_begin("unknown");
    sink.on_float("x", 1.0);
    sink.on_object_end("unknown");

    sink.on_float("speed", 30.0);
    REQUIRE(tp.speed == 30.0f);
    REQUIRE(tp.location.lat == 0.0);
}

TEST_CASE("StructSink deeply nested unknown objects are skipped") {
    TripPoint tp{};
    char buf[256];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));
    note::StructSink<TripPoint> sink(tp, pool);

    sink.on_object_begin("unknown");
    sink.on_object_begin("deep");
    sink.on_float("val", 1.0);
    sink.on_object_end("deep");
    sink.on_object_end("unknown");

    sink.on_float("speed", 50.0);
    REQUIRE(tp.speed == 50.0f);
}

TEST_CASE("StructSink parses array of primitives") {
    WithArrays wa{};
    char buf[256];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));
    note::StructSink<WithArrays> sink(wa, pool);

    sink.on_int("count", 3);
    sink.on_array_begin("temps");
    sink.on_float("", 22.5);
    sink.on_float("", 23.1);
    sink.on_float("", 21.8);
    sink.on_array_end("temps");

    REQUIRE(wa.count == 3);
    REQUIRE(wa.temps[0] == 22.5f);
    REQUIRE(wa.temps[1] == 23.1f);
    REQUIRE(wa.temps[2] == 21.8f);
    REQUIRE(wa.temps[3] == 0.0f);
}

TEST_CASE("StructSink parses array of structs") {
    WithStructArray wsa{};
    char buf[512];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));
    note::StructSink<WithStructArray> sink(wsa, pool);

    sink.on_int("count", 2);
    sink.on_array_begin("waypoints");
    sink.on_object_begin("");
    sink.on_float("lat", 42.36);
    sink.on_float("lon", -71.06);
    sink.on_object_end("");
    sink.on_object_begin("");
    sink.on_float("lat", 43.00);
    sink.on_float("lon", -72.00);
    sink.on_object_end("");
    sink.on_array_end("waypoints");

    REQUIRE(wsa.count == 2);
    REQUIRE(wsa.waypoints[0].lat == Approx(42.36));
    REQUIRE(wsa.waypoints[0].lon == Approx(-71.06));
    REQUIRE(wsa.waypoints[1].lat == Approx(43.00));
    REQUIRE(wsa.waypoints[1].lon == Approx(-72.00));
    REQUIRE(wsa.waypoints[2].lat == 0.0);
}

TEST_CASE("StructSink array overflow is safe") {
    WithArrays wa{};
    char buf[256];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));
    note::StructSink<WithArrays> sink(wa, pool);

    sink.on_array_begin("temps");
    sink.on_float("", 1.0);
    sink.on_float("", 2.0);
    sink.on_float("", 3.0);
    sink.on_float("", 4.0);
    sink.on_float("", 5.0);  // overflow
    sink.on_array_end("temps");

    REQUIRE(wa.temps[3] == 4.0f);
}

// ── BodyHandler tests ─────────────────────────────────────────────────

TEST_CASE("make_body_handler creates valid handler") {
    SensorData data{};
    char buf[256];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));
    note::StructSink<SensorData> sink(data, pool);

    auto handler = note::make_body_handler(sink);
    REQUIRE(static_cast<bool>(handler));
    REQUIRE(handler.ctx != nullptr);
    REQUIRE(handler.dispatch != nullptr);
}

TEST_CASE("BodyHandler forwards events to StructSink") {
    SensorData data{};
    char buf[256];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));
    note::StructSink<SensorData> sink(data, pool);

    auto handler = note::make_body_handler(sink);
    handler.send(note::BodyEvent::make_float("temperature", 22.5));
    handler.send(note::BodyEvent::make_int("humidity", 45));
    handler.send(note::BodyEvent::make_bool("active", true));

    REQUIRE(data.temperature == 22.5f);
    REQUIRE(data.humidity == 45);
    REQUIRE(data.active == true);
}

TEST_CASE("default BodyHandler is falsy") {
    note::BodyHandler handler{};
    REQUIRE_FALSE(static_cast<bool>(handler));
}

// ── into() on generated request types ────────────────────────────

#include <note/api/env_get.hpp>

namespace {

struct EnvBody {
    float temp;
    NOTE_FIELDS(temp)
};

struct EnvBodyFull {
    float temp;
    note::string_view label;
    NOTE_FIELDS(temp, label)
};

} // namespace

TEST_CASE("into() sets body handler factory on body-having endpoint") {
    EnvBody body{};
    note::api::EnvGet req;
    req.into(body);
    REQUIRE(req.body_ptr_ != nullptr);
    REQUIRE(req.body_handler_factory_ != nullptr);
}

TEST_CASE("body() alias works on endpoints without body request field") {
    EnvBody body{};
    note::api::EnvGet req;
    req.body(body);  // should compile — EnvGet has no body request field
    REQUIRE(req.body_ptr_ != nullptr);
}

TEST_CASE("into() alias works on body-having endpoint") {
    EnvBody body{};
    note::api::EnvGet req;
    req.into(body);
    REQUIRE(req.body_ptr_ != nullptr);
}

TEST_CASE("from() alias works on body-having endpoint") {
    EnvBody body{};
    note::api::EnvGet req;
    req.from(body);
    REQUIRE(req.body_ptr_ != nullptr);
}

TEST_CASE("body handler factory creates working StructSink") {
    EnvBodyFull body{};
    note::api::EnvGet req;
    req.into(body);

    // Call the factory to create the handler
    char buf[512];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));
    alignas(note::body_sink_storage_align)
        char storage[note::body_sink_storage_size];

    auto handler = req.body_handler_factory_(req.body_ptr_, pool, storage);
    REQUIRE(static_cast<bool>(handler));

    // Forward events through the handler
    handler.send(note::BodyEvent::make_float("temp", 22.5));
    handler.send(note::BodyEvent::make_string("label", "room-42"));

    REQUIRE(body.temp == 22.5f);
    REQUIRE(body.label == "room-42");
}

TEST_CASE("const into() returns copy, original unchanged") {
    EnvBody body1{};
    EnvBody body2{};
    const note::api::EnvGet req;

    auto r1 = req.into(body1);
    REQUIRE(r1.body_ptr_ != nullptr);
    REQUIRE(req.body_ptr_ == nullptr);  // original untouched

    auto r2 = req.into(body2);
    REQUIRE(r2.body_ptr_ == &body2);
    REQUIRE(r1.body_ptr_ == &body1);   // r1 still points to body1
    REQUIRE(req.body_ptr_ == nullptr);  // original still untouched
}

// ═══════════════════════════════════════════════════════════════════════
// GenericBodySink — table-driven body parsing (NOTE_RESPONSE_BODY=0 path)
// ═══════════════════════════════════════════════════════════════════════

#include <note/generic_sink.hpp>

namespace {
struct GBSTestData {
    float temperature;
    int32_t humidity;
    bool active;
    note::string_view label;
    NOTE_FIELDS(temperature, humidity, active, label)
};
} // namespace

TEST_CASE("_note_field_descs generates correct descriptor table") {
    uint8_t n = 0;
    auto* descs = GBSTestData::_note_field_descs<GBSTestData>(n);

    REQUIRE(n == 4);
    REQUIRE(descs[0].type == note::FieldType::Float32);   // float → Float32
    REQUIRE(descs[1].type == note::FieldType::Int);
    REQUIRE(descs[2].type == note::FieldType::Bool);
    REQUIRE(descs[3].type == note::FieldType::String);

    // Verify offsets are correct
    REQUIRE(descs[0].offset == offsetof(GBSTestData, temperature));
    REQUIRE(descs[1].offset == offsetof(GBSTestData, humidity));
    REQUIRE(descs[2].offset == offsetof(GBSTestData, active));
    REQUIRE(descs[3].offset == offsetof(GBSTestData, label));

    // Verify names
    REQUIRE(note::string_view(descs[0].name) == "temperature");
    REQUIRE(note::string_view(descs[1].name) == "humidity");
    REQUIRE(note::string_view(descs[2].name) == "active");
    REQUIRE(note::string_view(descs[3].name) == "label");
}

TEST_CASE("GenericBodySink dispatches all primitive types") {
    GBSTestData data{};
    char buf[256];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));

    uint8_t n = 0;
    auto* descs = GBSTestData::_note_field_descs<GBSTestData>(n);
    note::GenericBodySink sink{&data, descs, n, &pool};

    sink.on_float("temperature", 22.5);
    sink.on_int("humidity", 65);
    sink.on_bool("active", true);
    sink.on_string("label", "room-42");

    REQUIRE(data.temperature == 22.5f);
    REQUIRE(data.humidity == 65);
    REQUIRE(data.active == true);
    REQUIRE(std::string(data.label.data(), data.label.size()) == "room-42");
}

TEST_CASE("GenericBodySink handles int-to-float coercion") {
    GBSTestData data{};
    char buf[128];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));

    uint8_t n = 0;
    auto* descs = GBSTestData::_note_field_descs<GBSTestData>(n);
    note::GenericBodySink sink{&data, descs, n, &pool};

    // JSON parser may deliver integer for a float field
    sink.on_int("temperature", 23);
    REQUIRE(data.temperature == 23.0f);

    // And float for an integer field
    sink.on_float("humidity", 65.7);
    REQUIRE(data.humidity == 65);
}

TEST_CASE("GenericBodySink handles on_number") {
    GBSTestData data{};
    char buf[128];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));

    uint8_t n = 0;
    auto* descs = GBSTestData::_note_field_descs<GBSTestData>(n);
    note::GenericBodySink sink{&data, descs, n, &pool};

    sink.on_number("temperature", "22.5");
    sink.on_number("humidity", "65");

    REQUIRE(data.temperature == 22.5f);
    REQUIRE(data.humidity == 65);
}

TEST_CASE("GenericBodySink ignores unknown fields") {
    GBSTestData data{};
    data.humidity = 42;
    char buf[128];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));

    uint8_t n = 0;
    auto* descs = GBSTestData::_note_field_descs<GBSTestData>(n);
    note::GenericBodySink sink{&data, descs, n, &pool};

    sink.on_int("nonexistent", 999);
    REQUIRE(data.humidity == 42);  // unchanged
}

TEST_CASE("GenericBodySink reset() zeros all fields") {
    GBSTestData data{23.5f, 65, true, "hello"};
    char buf[128];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));

    uint8_t n = 0;
    auto* descs = GBSTestData::_note_field_descs<GBSTestData>(n);
    note::GenericBodySink sink{&data, descs, n, &pool};

    sink.reset();
    REQUIRE(data.temperature == 0.0f);
    REQUIRE(data.humidity == 0);
    REQUIRE(data.active == false);
    REQUIRE(data.label.empty());
}

TEST_CASE("make_generic_body_handler round-trip via BodyEvent") {
    GBSTestData data{};
    char buf[256];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));

    uint8_t n = 0;
    auto* descs = GBSTestData::_note_field_descs<GBSTestData>(n);
    note::GenericBodySink sink{&data, descs, n, &pool};

    auto handler = note::make_generic_body_handler(sink);
    REQUIRE(static_cast<bool>(handler));

    handler.send(note::BodyEvent::make_float("temperature", 22.5));
    handler.send(note::BodyEvent::make_int("humidity", 65));
    handler.send(note::BodyEvent::make_bool("active", true));
    handler.send(note::BodyEvent::make_string("label", "sensor-1"));

    REQUIRE(data.temperature == 22.5f);
    REQUIRE(data.humidity == 65);
    REQUIRE(data.active == true);
    REQUIRE(std::string(data.label.data(), data.label.size()) == "sensor-1");
}

// ── Additional branch coverage tests ─────────────────────────────────

namespace {
struct WithIntArray {
    std::array<int32_t, 4> values;
    NOTE_FIELDS(values)
};

struct WithStringArray {
    std::array<note::string_view, 3> tags;
    NOTE_FIELDS(tags)
};
} // namespace

TEST_CASE("StructSink: int-to-float coercion in float array") {
    WithArrays wa{};
    char buf[256];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));
    note::StructSink<WithArrays> sink(wa, pool);

    sink.on_array_begin("temps");
    sink.on_int("", 22);   // int → float coercion
    sink.on_int("", 23);
    sink.on_array_end("temps");

    REQUIRE(wa.temps[0] == 22.0f);
    REQUIRE(wa.temps[1] == 23.0f);
}

TEST_CASE("StructSink: float-to-int coercion in int array") {
    WithIntArray wa{};
    char buf[256];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));
    note::StructSink<WithIntArray> sink(wa, pool);

    sink.on_array_begin("values");
    sink.on_float("", 42.7);   // float → int coercion (truncation)
    sink.on_float("", 99.1);
    sink.on_array_end("values");

    REQUIRE(wa.values[0] == 42);
    REQUIRE(wa.values[1] == 99);
}

TEST_CASE("StructSink: on_number with raw string for int array") {
    WithIntArray wa{};
    char buf[256];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));
    note::StructSink<WithIntArray> sink(wa, pool);

    sink.on_array_begin("values");
    sink.on_number("", "42");
    sink.on_number("", "99");
    sink.on_array_end("values");

    REQUIRE(wa.values[0] == 42);
    REQUIRE(wa.values[1] == 99);
}

TEST_CASE("StructSink: string array elements") {
    WithStringArray ws{};
    char buf[512];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));
    note::StructSink<WithStringArray> sink(ws, pool);

    sink.on_array_begin("tags");
    sink.on_string("", "alpha");
    sink.on_string("", "beta");
    sink.on_array_end("tags");

    REQUIRE(ws.tags[0] == "alpha");
    REQUIRE(ws.tags[1] == "beta");
    REQUIRE(ws.tags[2].empty());
}

TEST_CASE("StructSink: unknown array field sets skip") {
    SensorData data{};
    data.humidity = 42;
    char buf[256];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));
    note::StructSink<SensorData> sink(data, pool);

    sink.on_array_begin("nonexistent");
    sink.on_int("", 999);
    sink.on_string("", "junk");
    sink.on_array_end("nonexistent");

    REQUIRE(data.humidity == 42); // unchanged
}

TEST_CASE("StructSink: primitive array receives object → skip") {
    WithArrays wa{};
    char buf[256];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));
    note::StructSink<WithArrays> sink(wa, pool);

    sink.on_array_begin("temps");
    sink.on_float("", 1.0);
    sink.on_object_begin("");  // unexpected object in primitive array → skip
    sink.on_float("x", 999.0);
    sink.on_object_end("");
    sink.on_float("", 2.0);
    sink.on_array_end("temps");

    REQUIRE(wa.temps[0] == 1.0f);
    REQUIRE(wa.temps[1] == 2.0f);
}

TEST_CASE("StructSink: struct array at capacity receives object → skip") {
    WithStructArray wsa{};
    char buf[512];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));
    note::StructSink<WithStructArray> sink(wsa, pool);

    sink.on_array_begin("waypoints");
    // Fill all 3 slots
    for (int i = 0; i < 3; ++i) {
        sink.on_object_begin("");
        sink.on_float("lat", 10.0 + i);
        sink.on_float("lon", 20.0 + i);
        sink.on_object_end("");
    }
    // 4th object → at capacity, should skip
    sink.on_object_begin("");
    sink.on_float("lat", 999.0);
    sink.on_object_end("");
    sink.on_array_end("waypoints");

    REQUIRE(wsa.waypoints[2].lat == Approx(12.0));
}

TEST_CASE("StructSink: array in skip context increments skip_depth") {
    TripPoint tp{};
    char buf[256];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));
    note::StructSink<TripPoint> sink(tp, pool);

    // Enter unknown object → skip_depth = 1
    sink.on_object_begin("unknown");
    // Array inside unknown object → skip_depth increments
    sink.on_array_begin("nested_array");
    sink.on_int("", 42);
    sink.on_array_end("nested_array");
    sink.on_object_end("unknown");

    sink.on_float("speed", 55.0);
    REQUIRE(tp.speed == 55.0f);
}

TEST_CASE("StructSink: events in skip context are ignored") {
    SensorData data{};
    char buf[256];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));
    note::StructSink<SensorData> sink(data, pool);

    // Enter unknown object → skip
    sink.on_object_begin("unknown");
    sink.on_bool("active", true);       // should be skipped
    sink.on_int("humidity", 99);        // should be skipped
    sink.on_float("temperature", 99.9); // should be skipped
    sink.on_string("x", "y");           // should be skipped
    sink.on_number("temperature", "99");// should be skipped
    sink.on_object_end("unknown");

    REQUIRE(data.temperature == 0.0f);
    REQUIRE(data.humidity == 0);
    REQUIRE(data.active == false);
}

TEST_CASE("StructSink: on_null is no-op") {
    SensorData data{};
    data.humidity = 42;
    char buf[256];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));
    note::StructSink<SensorData> sink(data, pool);

    sink.on_null("humidity");
    sink.on_null("unknown");

    REQUIRE(data.humidity == 42); // unchanged
}

namespace {
struct WithBoolArray {
    std::array<bool, 3> flags;
    NOTE_FIELDS(flags)
};
} // namespace

TEST_CASE("StructSink: bool array elements") {
    WithBoolArray wa{};
    char buf[256];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));
    note::StructSink<WithBoolArray> sink(wa, pool);

    sink.on_array_begin("flags");
    sink.on_bool("", true);
    sink.on_bool("", false);
    sink.on_bool("", true);
    sink.on_array_end("flags");

    REQUIRE(wa.flags[0] == true);
    REQUIRE(wa.flags[1] == false);
    REQUIRE(wa.flags[2] == true);
}

TEST_CASE("make_generic_body_handler: ignored event types") {
    GBSTestData data{};
    data.humidity = 42;
    char buf[256];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));

    uint8_t n = 0;
    auto* descs = GBSTestData::_note_field_descs<GBSTestData>(n);
    note::GenericBodySink sink{&data, descs, n, &pool};

    auto handler = note::make_generic_body_handler(sink);

    // These events should be silently ignored (default: break).
    handler.send(note::BodyEvent::make_object_begin("nested"));
    handler.send(note::BodyEvent::make_object_end("nested"));
    handler.send(note::BodyEvent::make_array_begin("items"));
    handler.send(note::BodyEvent::make_array_end("items"));
    handler.send(note::BodyEvent::make_reset());

    REQUIRE(data.humidity == 42); // unchanged (reset goes to default: break)
}

TEST_CASE("field_type_of maps C++ types correctly") {
    STATIC_REQUIRE(note::field_type_of<bool>() == note::FieldType::Bool);
    STATIC_REQUIRE(note::field_type_of<int8_t>() == note::FieldType::Int8);
    STATIC_REQUIRE(note::field_type_of<uint8_t>() == note::FieldType::Int8);
    STATIC_REQUIRE(note::field_type_of<int16_t>() == note::FieldType::Int16);
    STATIC_REQUIRE(note::field_type_of<uint16_t>() == note::FieldType::Int16);
    STATIC_REQUIRE(note::field_type_of<int32_t>() == note::FieldType::Int);
    STATIC_REQUIRE(note::field_type_of<float>() == note::FieldType::Float32);
    STATIC_REQUIRE(note::field_type_of<double>() == note::FieldType::Double);
    STATIC_REQUIRE(note::field_type_of<note::string_view>() == note::FieldType::String);
}

// ═══════════════════════════════════════════════════════════════════════
// child_ctx_ forwarding — all event types through nested struct
// ═══════════════════════════════════════════════════════════════════════

namespace {

// A nested struct with ALL field types to exercise child_ctx_ forwarding.
struct ChildAllTypes {
    bool active;
    int32_t count;
    float temp;
    note::string_view label;
    std::array<int32_t, 3> values;
    NOTE_FIELDS(active, count, temp, label, values)
};

struct ParentWithChild {
    ChildAllTypes child;
    int32_t parent_val;
    NOTE_FIELDS(child, parent_val)
};

// Doubly nested: parent -> mid -> inner
struct Inner {
    float x;
    note::string_view name;
    NOTE_FIELDS(x, name)
};

struct Mid {
    Inner inner;
    int32_t y;
    NOTE_FIELDS(inner, y)
};

struct DoublyNested {
    Mid mid;
    bool top;
    NOTE_FIELDS(mid, top)
};

} // namespace

TEST_CASE("StructSink: child_ctx_ forwards on_bool") {
    ParentWithChild obj{};
    char buf[1024];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));
    note::StructSink<ParentWithChild> sink(obj, pool);

    sink.on_object_begin("child");
    sink.on_bool("active", true);
    sink.on_object_end("child");

    REQUIRE(obj.child.active == true);
}

TEST_CASE("StructSink: child_ctx_ forwards on_int") {
    ParentWithChild obj{};
    char buf[1024];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));
    note::StructSink<ParentWithChild> sink(obj, pool);

    sink.on_object_begin("child");
    sink.on_int("count", 42);
    sink.on_object_end("child");

    REQUIRE(obj.child.count == 42);
}

TEST_CASE("StructSink: child_ctx_ forwards on_float") {
    ParentWithChild obj{};
    char buf[1024];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));
    note::StructSink<ParentWithChild> sink(obj, pool);

    sink.on_object_begin("child");
    sink.on_float("temp", 22.5);
    sink.on_object_end("child");

    REQUIRE(obj.child.temp == 22.5f);
}

TEST_CASE("StructSink: child_ctx_ forwards on_string") {
    ParentWithChild obj{};
    char buf[1024];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));
    note::StructSink<ParentWithChild> sink(obj, pool);

    sink.on_object_begin("child");
    sink.on_string("label", "sensor-1");
    sink.on_object_end("child");

    REQUIRE(obj.child.label == "sensor-1");
}

TEST_CASE("StructSink: child_ctx_ forwards on_number") {
    ParentWithChild obj{};
    char buf[1024];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));
    note::StructSink<ParentWithChild> sink(obj, pool);

    sink.on_object_begin("child");
    sink.on_number("count", "99");
    sink.on_number("temp", "22.5");
    sink.on_object_end("child");

    REQUIRE(obj.child.count == 99);
    REQUIRE(obj.child.temp == 22.5f);
}

TEST_CASE("StructSink: child_ctx_ forwards on_array_begin/end") {
    ParentWithChild obj{};
    char buf[1024];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));
    note::StructSink<ParentWithChild> sink(obj, pool);

    sink.on_object_begin("child");
    sink.on_array_begin("values");
    sink.on_int("", 10);
    sink.on_int("", 20);
    sink.on_int("", 30);
    sink.on_array_end("values");
    sink.on_object_end("child");

    REQUIRE(obj.child.values[0] == 10);
    REQUIRE(obj.child.values[1] == 20);
    REQUIRE(obj.child.values[2] == 30);
}

TEST_CASE("StructSink: doubly nested struct — child_depth > 1 forwarding") {
    DoublyNested obj{};
    char buf[1024];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));
    note::StructSink<DoublyNested> sink(obj, pool);

    sink.on_bool("top", true);
    sink.on_object_begin("mid");     // child_ctx_ set, child_depth_ = 1
    sink.on_int("y", 42);

    // Nested object inside child — child_depth_ goes to 2
    sink.on_object_begin("inner");   // child_depth_ = 2
    sink.on_float("x", 3.14);
    sink.on_string("name", "deep");
    sink.on_object_end("inner");     // child_depth_ back to 1, inner cleared
    sink.on_object_end("mid");       // child_depth_ = 0, child_ctx_ cleared

    REQUIRE(obj.top == true);
    REQUIRE(obj.mid.y == 42);
    REQUIRE(obj.mid.inner.x == Approx(3.14f));
    REQUIRE(obj.mid.inner.name == "deep");
}

TEST_CASE("StructSink: child_ctx_ on_object_begin increments child_depth") {
    DoublyNested obj{};
    char buf[1024];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));
    note::StructSink<DoublyNested> sink(obj, pool);

    sink.on_object_begin("mid");
    sink.on_object_begin("unknown_nested");
    sink.on_int("something", 999);
    sink.on_object_end("unknown_nested");

    sink.on_object_begin("inner");
    sink.on_float("x", 1.0);
    sink.on_object_end("inner");
    sink.on_object_end("mid");

    REQUIRE(obj.mid.inner.x == 1.0f);
    REQUIRE(obj.mid.y == 0);
}


// ═══════════════════════════════════════════════════════════════════════
// Array overflow for all element types (null elem paths)
// ═══════════════════════════════════════════════════════════════════════

TEST_CASE("StructSink: bool array overflow is safe") {
    WithBoolArray wa{};
    char buf[256];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));
    note::StructSink<WithBoolArray> sink(wa, pool);

    sink.on_array_begin("flags");
    sink.on_bool("", true);
    sink.on_bool("", false);
    sink.on_bool("", true);
    sink.on_bool("", true);  // overflow — capacity is 3
    sink.on_array_end("flags");

    REQUIRE(wa.flags[2] == true);
}

TEST_CASE("StructSink: int array overflow is safe") {
    WithIntArray wa{};
    char buf[256];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));
    note::StructSink<WithIntArray> sink(wa, pool);

    sink.on_array_begin("values");
    sink.on_int("", 1);
    sink.on_int("", 2);
    sink.on_int("", 3);
    sink.on_int("", 4);
    sink.on_int("", 5); // overflow — capacity is 4
    sink.on_array_end("values");

    REQUIRE(wa.values[3] == 4);
}

TEST_CASE("StructSink: string array overflow is safe") {
    WithStringArray ws{};
    char buf[512];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));
    note::StructSink<WithStringArray> sink(ws, pool);

    sink.on_array_begin("tags");
    sink.on_string("", "a");
    sink.on_string("", "b");
    sink.on_string("", "c");
    sink.on_string("", "overflow"); // overflow — capacity is 3
    sink.on_array_end("tags");

    REQUIRE(ws.tags[2] == "c");
}

TEST_CASE("StructSink: number in array overflow is safe") {
    WithIntArray wa{};
    char buf[256];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));
    note::StructSink<WithIntArray> sink(wa, pool);

    sink.on_array_begin("values");
    sink.on_number("", "10");
    sink.on_number("", "20");
    sink.on_number("", "30");
    sink.on_number("", "40");
    sink.on_number("", "50"); // overflow — capacity is 4
    sink.on_array_end("values");

    REQUIRE(wa.values[3] == 40);
}

// ═══════════════════════════════════════════════════════════════════════
// on_array_end with child_ctx_ forwarding (line 404)
// ═══════════════════════════════════════════════════════════════════════

namespace {

struct ChildWithArray {
    std::array<int32_t, 3> nums;
    NOTE_FIELDS(nums)
};

struct ParentOfChildWithArray {
    ChildWithArray sub;
    int32_t extra;
    NOTE_FIELDS(sub, extra)
};

} // namespace

TEST_CASE("StructSink: array inside nested struct — child forwards array events") {
    ParentOfChildWithArray obj{};
    char buf[1024];
    note::MonotonicArena arena(buf);
    note::StringPool pool(note::arena_allocator(arena));
    note::StructSink<ParentOfChildWithArray> sink(obj, pool);

    sink.on_int("extra", 7);
    sink.on_object_begin("sub");
    sink.on_array_begin("nums");
    sink.on_int("", 10);
    sink.on_int("", 20);
    sink.on_int("", 30);
    sink.on_array_end("nums");
    sink.on_object_end("sub");

    REQUIRE(obj.extra == 7);
    REQUIRE(obj.sub.nums[0] == 10);
    REQUIRE(obj.sub.nums[1] == 20);
    REQUIRE(obj.sub.nums[2] == 30);
}
