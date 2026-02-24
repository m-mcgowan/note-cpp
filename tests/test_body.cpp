// Tests for BodyValue: string, builder, schema, and response body parsing.
#include "catch.hpp"
#include "test_json_backend.hpp"

#include <note/body.hpp>
#include <note/notecard.hpp>
#include <note/api_context.hpp>
#include <memory>
#include <map>
#include <string>
#include <variant>

namespace {

// Minimal request type that uses BodyValue for its body field.
struct TestRequest {
    static constexpr note::string_view notecard_request = "test.req";
    static constexpr bool supports_cmd = false;
    static constexpr note::Safety safety = note::Safety::Idempotent;

    note::BodyValue body{};

    struct Response {
        static Response parse(std::unique_ptr<note::JsonReader>) { return {}; }
    };

    void build(note::JsonBuilder& b) const {
        body.write_to(b);
    }
};

struct TestHarness {
    note::test::TestJsonBackend backend;
    note::test::CapturingIO io;
    note::Notecard nc{backend, io};
};

// ── Schema test types ───────────────────────────────────────────────────────

struct Readings {
    float temperature;
    int16_t humidity;
};

struct SensorData {
    double voltage;
    bool active;
    int32_t count;
};

// C++17 fallback: macro-registered schema
struct MacroReadings {
    float temperature;
    int16_t humidity;
    NOTE_BODY(temperature, humidity)
};

} // namespace

// ── Tier 1: Raw JSON string ─────────────────────────────────────────────────

TEST_CASE("BodyValue from raw JSON string") {
    TestHarness h;
    TestRequest req;
    req.body = R"({"temp":22.5,"humidity":60})";
    h.nc.execute(req);
    REQUIRE(h.io.last_request ==
        R"({"req":"test.req","body":"{\"temp\":22.5,\"humidity\":60}"})");
}

TEST_CASE("BodyValue default is empty") {
    TestHarness h;
    TestRequest req;
    h.nc.execute(req);
    REQUIRE(h.io.last_request == R"({"req":"test.req"})");
}

// ── Tier 2: Builder (lambda) ────────────────────────────────────────────────

TEST_CASE("BodyValue from builder lambda") {
    TestHarness h;
    TestRequest req;
    req.body = note::body([](note::JsonBuilder& b) {
        b.add("temp", 22.5);
        b.add("humidity", int32_t{60});
    });
    h.nc.execute(req);
    REQUIRE(h.io.last_request ==
        R"({"req":"test.req","body":{"temp":22.5,"humidity":60}})");
}

TEST_CASE("BodyValue from builder with nested objects") {
    TestHarness h;
    TestRequest req;
    req.body = note::body([](note::JsonBuilder& b) {
        b.add("temp", 22.5);
        b.begin_object("location");
            b.add("lat", 42.36);
            b.add("lon", -71.06);
        b.end_object();
    });
    h.nc.execute(req);
    REQUIRE(h.io.last_request ==
        R"({"req":"test.req","body":{"temp":22.5,"location":{"lat":42.36,"lon":-71.06}}})");
}

// ── Tier 3: Schema (C++20 reflection) ───────────────────────────────────────

#if __cplusplus >= 202002L

TEST_CASE("BodyValue from reflected aggregate") {
    TestHarness h;
    TestRequest req;
    Readings r{.temperature = 22.5f, .humidity = 60};
    req.body = note::make_schema_body(r);
    h.nc.execute(req);
    REQUIRE(h.io.last_request ==
        R"({"req":"test.req","body":{"temperature":22.5,"humidity":60}})");
}

TEST_CASE("BodyValue from reflected aggregate with mixed types") {
    TestHarness h;
    TestRequest req;
    SensorData sd{.voltage = 3.3, .active = true, .count = 42};
    req.body = note::make_schema_body(sd);
    h.nc.execute(req);
    REQUIRE(h.io.last_request ==
        R"({"req":"test.req","body":{"voltage":3.3,"active":true,"count":42}})");
}

TEST_CASE("template_of generates type hints") {
    TestHarness h;
    TestRequest req;
    req.body = note::template_of<Readings>();
    h.nc.execute(req);
    REQUIRE(h.io.last_request ==
        R"({"req":"test.req","body":{"temperature":14.1,"humidity":11}})");
}

TEST_CASE("template_of with mixed types") {
    TestHarness h;
    TestRequest req;
    req.body = note::template_of<SensorData>();
    h.nc.execute(req);
    REQUIRE(h.io.last_request ==
        R"({"req":"test.req","body":{"voltage":14.1,"active":true,"count":12}})");
}

#endif // C++20

// ── NOTE_BODY macro ─────────────────────────────────────────────────────────

TEST_CASE("BodyValue from NOTE_BODY macro schema") {
    TestHarness h;
    TestRequest req;
    MacroReadings r{.temperature = 22.5f, .humidity = 60};
    req.body = note::make_schema_body(r);
    h.nc.execute(req);
    REQUIRE(h.io.last_request ==
        R"({"req":"test.req","body":{"temperature":22.5,"humidity":60}})");
}

// ── Integration with generated API types ────────────────────────────────────

#include <note/api/note_add.hpp>
#include <note/api/note_template.hpp>

TEST_CASE("note.add with string body") {
    TestHarness h;
    note::Api api(h.nc);
    auto req = api.noteAdd();
    req.file = "sensors.qo";
    req.body = R"({"temp":22.5})";
    req.execute();
    REQUIRE(h.io.last_request ==
        R"({"req":"note.add","body":"{\"temp\":22.5}","file":"sensors.qo"})");
}

#if __cplusplus >= 202002L

TEST_CASE("note.add with reflected schema body") {
    TestHarness h;
    note::Api api(h.nc);
    Readings r{.temperature = 22.5f, .humidity = 60};
    api.noteAdd().set_file("sensors.qo").set_body(r).execute();
    REQUIRE(h.io.last_request ==
        R"({"req":"note.add","body":{"temperature":22.5,"humidity":60},"file":"sensors.qo"})");
}

TEST_CASE("note.template with template_of") {
    TestHarness h;
    note::Api api(h.nc);
    api.noteTemplate().set()
        .set_file("sensors.qo")
        .set_body(note::template_of<Readings>())
        .execute();
    REQUIRE(h.io.last_request ==
        R"({"req":"note.template","body":{"temperature":14.1,"humidity":11},"file":"sensors.qo"})");
}

TEST_CASE("note.add with builder body") {
    TestHarness h;
    note::Api api(h.nc);
    api.noteAdd()
        .set_file("sensors.qo")
        .set_body(note::body([](note::JsonBuilder& b) {
            b.add("temp", 22.5);
            b.add("count", int32_t{42});
        }))
        .execute();
    REQUIRE(h.io.last_request ==
        R"({"req":"note.add","body":{"temp":22.5,"count":42},"file":"sensors.qo"})");
}

#endif // C++20

// ── Response body parsing ───────────────────────────────────────────────────

#include <note/api/note_get.hpp>

TEST_CASE("note.get response body() returns null when no body") {
    auto reader = std::make_unique<note::test::PopulatedJsonReader>();
    reader->set("time", int32_t{1234567890});
    auto rsp = note::api::NoteGet::Query::Response::parse(std::move(reader));
    REQUIRE(rsp.time == 1234567890);
    REQUIRE(rsp.body() == nullptr);
}

TEST_CASE("note.get response body() returns reader when body present") {
    auto body = std::make_unique<note::test::PopulatedJsonReader>();
    body->set("temperature", 22.5);
    body->set("humidity", int32_t{60});

    auto reader = std::make_unique<note::test::PopulatedJsonReader>();
    reader->set("time", int32_t{1234567890});
    reader->set_object("body", std::move(body));

    auto rsp = note::api::NoteGet::Query::Response::parse(std::move(reader));
    REQUIRE(rsp.time == 1234567890);
    REQUIRE(rsp.body() != nullptr);
    REQUIRE(rsp.body()->get_double("temperature") == 22.5);
    REQUIRE(rsp.body()->get_int("humidity") == 60);
}

#if __cplusplus >= 202002L

TEST_CASE("note.get response body_as<T>() with reflected struct") {
    auto body = std::make_unique<note::test::PopulatedJsonReader>();
    body->set("temperature", 22.5);
    body->set("humidity", int32_t{60});

    auto reader = std::make_unique<note::test::PopulatedJsonReader>();
    reader->set_object("body", std::move(body));

    auto rsp = note::api::NoteGet::Query::Response::parse(std::move(reader));
    auto r = rsp.body_as<Readings>();
    REQUIRE(r.temperature == 22.5f);
    REQUIRE(r.humidity == 60);
}

TEST_CASE("note.get response body_as<T>() returns default when no body") {
    auto reader = std::make_unique<note::test::PopulatedJsonReader>();
    auto rsp = note::api::NoteGet::Query::Response::parse(std::move(reader));
    auto r = rsp.body_as<Readings>();
    REQUIRE(r.temperature == 0.0f);
    REQUIRE(r.humidity == 0);
}

TEST_CASE("note.get response body_as<T>() with mixed types") {
    auto body = std::make_unique<note::test::PopulatedJsonReader>();
    body->set("voltage", 3.3);
    body->set("active", true);
    body->set("count", int32_t{42});

    auto reader = std::make_unique<note::test::PopulatedJsonReader>();
    reader->set_object("body", std::move(body));

    auto rsp = note::api::NoteGet::Query::Response::parse(std::move(reader));
    auto sd = rsp.body_as<SensorData>();
    REQUIRE(sd.voltage == 3.3);
    REQUIRE(sd.active == true);
    REQUIRE(sd.count == 42);
}

#endif // C++20

// ── NOTE_BODY macro response parsing ────────────────────────────────────────

TEST_CASE("note.get response body_as<T>() with NOTE_BODY macro type") {
    auto body = std::make_unique<note::test::PopulatedJsonReader>();
    body->set("temperature", 22.5);
    body->set("humidity", int32_t{60});

    auto reader = std::make_unique<note::test::PopulatedJsonReader>();
    reader->set_object("body", std::move(body));

    auto rsp = note::api::NoteGet::Query::Response::parse(std::move(reader));
    auto r = rsp.body_as<MacroReadings>();
    REQUIRE(r.temperature == 22.5f);
    REQUIRE(r.humidity == 60);
}

TEST_CASE("parse<T>() with NOTE_BODY macro type") {
    auto reader = std::make_unique<note::test::PopulatedJsonReader>();
    reader->set("temperature", 22.5);
    reader->set("humidity", int32_t{60});

    auto r = note::parse<MacroReadings>(*reader);
    REQUIRE(r.temperature == 22.5f);
    REQUIRE(r.humidity == 60);
}
