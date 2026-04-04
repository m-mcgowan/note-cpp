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
