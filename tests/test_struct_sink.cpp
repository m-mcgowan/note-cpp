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
