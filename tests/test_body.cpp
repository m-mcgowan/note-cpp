// Tests for BodyValue: string, builder, schema, and response body parsing.
#include "catch.hpp"
#include "test_json_backend.hpp"

#include <note/body.hpp>
#include <note/notecard.hpp>
#include <note/api.hpp>
#include <memory>
#include <map>
#include <string>
#include <variant>

namespace {

// Minimal request type that uses BodyValue for its body field.
struct TestRequest {
    static constexpr note::string_view notecard_request = "test.req";
    [[maybe_unused]] static constexpr bool supports_cmd = false;
    [[maybe_unused]] static constexpr note::Safety safety = note::Safety::Idempotent;

    note::BodyValue body{};

    struct Response {
        static Response parse(std::unique_ptr<note::JsonReader>) { return {}; }
        static Response parse(const note::JsonReader&) { return {}; }
    };

    void build(note::JsonBuilder& b) const {
        body.write_to(b);
    }
};

struct TestHarness {
    note::test::TestJsonBackend backend;
    std::string last_request;
    std::string last_response{"{}"}; // persists for string_view return
    note::CallbackTransport transport;
    note::Notecard nc;

    TestHarness()
        : transport(
            [this](note::string_view req, uint32_t) -> note::Result<note::string_view> {
                last_request = std::string(req);
                last_response = "{}";
                return note::string_view(last_response);
            },
            [this](note::string_view req) -> note::Result<void> {
                last_request = std::string(req);
                return {};
            })
        , nc(backend, transport) {}
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
    NOTE_FIELDS(temperature, humidity)
};

// Type hint coverage: one field per supported integer size
struct AllIntSizes {
    int8_t  tiny;
    int16_t small_field;
    int32_t medium;
    int64_t large;
};

// Type hint coverage: string field
struct WithStringField {
    float       value;
    std::string label;
};

// Nested aggregate: exercises write_field<V> ReflectableAggregate branch.
struct GpsPos { double lat; double lon; };
struct SensorWithLocation { float temp; GpsPos pos; };

} // namespace

// ── Tier 1: Raw JSON string ─────────────────────────────────────────────────

TEST_CASE("BodyValue from raw JSON string") {
    TestHarness h;
    TestRequest req;
    req.body = R"({"temp":22.5,"humidity":60})";
    h.nc.execute(req);
    REQUIRE(h.last_request ==
        R"({"req":"test.req","body":"{\"temp\":22.5,\"humidity\":60}"})");
}

TEST_CASE("BodyValue default is empty") {
    TestHarness h;
    TestRequest req;
    h.nc.execute(req);
    REQUIRE(h.last_request == R"({"req":"test.req"})");
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
    REQUIRE(h.last_request ==
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
    REQUIRE(h.last_request ==
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
    REQUIRE(h.last_request ==
        R"({"req":"test.req","body":{"temperature":22.5,"humidity":60}})");
}

TEST_CASE("BodyValue from reflected aggregate with mixed types") {
    TestHarness h;
    TestRequest req;
    SensorData sd{.voltage = 3.3, .active = true, .count = 42};
    req.body = note::make_schema_body(sd);
    h.nc.execute(req);
    REQUIRE(h.last_request ==
        R"({"req":"test.req","body":{"voltage":3.3,"active":true,"count":42}})");
}

TEST_CASE("template_of generates type hints") {
    TestHarness h;
    TestRequest req;
    req.body = note::template_of<Readings>();
    h.nc.execute(req);
    REQUIRE(h.last_request ==
        R"({"req":"test.req","body":{"temperature":14.1,"humidity":11}})");
}

TEST_CASE("template_of with mixed types") {
    TestHarness h;
    TestRequest req;
    req.body = note::template_of<SensorData>();
    h.nc.execute(req);
    REQUIRE(h.last_request ==
        R"({"req":"test.req","body":{"voltage":14.1,"active":true,"count":12}})");
}

TEST_CASE("template_of integer size hints: int8→1, int16→11, int32→12, int64→12") {
    TestHarness h;
    TestRequest req;
    req.body = note::template_of<AllIntSizes>();
    h.nc.execute(req);
    REQUIRE(h.last_request ==
        R"({"req":"test.req","body":{"tiny":1,"small_field":11,"medium":12,"large":12}})");
}

TEST_CASE("template_of string field hint → \"1\"") {
    TestHarness h;
    TestRequest req;
    req.body = note::template_of<WithStringField>();
    h.nc.execute(req);
    REQUIRE(h.last_request ==
        R"({"req":"test.req","body":{"value":14.1,"label":"1"}})");
}

TEST_CASE("BodyValue from reflected aggregate with nested aggregate field") {
    TestHarness h;
    TestRequest req;
    SensorWithLocation s{.temp = 21.0f, .pos = {.lat = 42.36, .lon = -71.06}};
    req.body = note::make_schema_body(s);
    h.nc.execute(req);
    REQUIRE(h.last_request ==
        R"({"req":"test.req","body":{"temp":21,"pos":{"lat":42.36,"lon":-71.06}}})");
}

TEST_CASE("note.template verify:true includes verify field in request") {
    TestHarness h;
    note::Api api(h.nc);
    api.note.templates().define("sensors.qo")
        .body(note::template_of<Readings>())
        .verify(true)
        .execute();
    REQUIRE(h.last_request ==
        R"({"req":"note.template","body":{"temperature":14.1,"humidity":11},"file":"sensors.qo","verify":true})");
}

#endif // C++20

// ── NOTE_FIELDS macro ────────────────────────────────────────────────────────

TEST_CASE("BodyValue from NOTE_FIELDS macro schema") {
    TestHarness h;
    TestRequest req;
    MacroReadings r{.temperature = 22.5f, .humidity = 60};
    req.body = note::make_schema_body(r);
    h.nc.execute(req);
    REQUIRE(h.last_request ==
        R"({"req":"test.req","body":{"temperature":22.5,"humidity":60}})");
}

// ── Integration with generated API types ────────────────────────────────────

#include <note/api/note_add.hpp>
#include <note/api/note_template.hpp>

TEST_CASE("note.add with string body") {
    TestHarness h;
    note::Api api(h.nc);
    auto req = api.note.add();
    req.file = "sensors.qo";
    req.body = R"({"temp":22.5})";
    req.execute();
    REQUIRE(h.last_request ==
        R"({"req":"note.add","body":"{\"temp\":22.5}","file":"sensors.qo"})");
}

#if __cplusplus >= 202002L

TEST_CASE("note.add with reflected schema body") {
    TestHarness h;
    note::Api api(h.nc);
    Readings r{.temperature = 22.5f, .humidity = 60};
    api.note.add().file("sensors.qo").body(r).execute();
    REQUIRE(h.last_request ==
        R"({"req":"note.add","body":{"temperature":22.5,"humidity":60},"file":"sensors.qo"})");
}

TEST_CASE("note.template with template_of") {
    TestHarness h;
    note::Api api(h.nc);
    api.note.templates().define("sensors.qo")
        .body(note::template_of<Readings>())
        .execute();
    REQUIRE(h.last_request ==
        R"({"req":"note.template","body":{"temperature":14.1,"humidity":11},"file":"sensors.qo"})");
}

TEST_CASE("note.add with builder body") {
    TestHarness h;
    note::Api api(h.nc);
    api.note.add()
        .file("sensors.qo")
        .body(note::body([](note::JsonBuilder& b) {
            b.add("temp", 22.5);
            b.add("count", int32_t{42});
        }))
        .execute();
    REQUIRE(h.last_request ==
        R"({"req":"note.add","body":{"temp":22.5,"count":42},"file":"sensors.qo"})");
}

#endif // C++20

// ── Response body parsing ───────────────────────────────────────────────────

#include <note/api/note_get.hpp>

TEST_CASE("note.get response body() returns null when no body") {
    auto reader = std::make_unique<note::test::PopulatedJsonReader>();
    reader->set("time", int32_t{1234567890});
    auto rsp = note::api::NoteGet::Get::Response::parse(std::move(reader));
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

    auto rsp = note::api::NoteGet::Get::Response::parse(std::move(reader));
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

    auto rsp = note::api::NoteGet::Get::Response::parse(std::move(reader));
    auto r = rsp.bodyAs<Readings>();
    REQUIRE(r.temperature == 22.5f);
    REQUIRE(r.humidity == 60);
}

TEST_CASE("note.get response body_as<T>() returns default when no body") {
    auto reader = std::make_unique<note::test::PopulatedJsonReader>();
    auto rsp = note::api::NoteGet::Get::Response::parse(std::move(reader));
    auto r = rsp.bodyAs<Readings>();
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

    auto rsp = note::api::NoteGet::Get::Response::parse(std::move(reader));
    auto sd = rsp.bodyAs<SensorData>();
    REQUIRE(sd.voltage == 3.3);
    REQUIRE(sd.active == true);
    REQUIRE(sd.count == 42);
}

#endif // C++20

// ── NOTE_FIELDS macro response parsing ───────────────────────────────────────

TEST_CASE("note.get response body_as<T>() with NOTE_FIELDS macro type") {
    auto body = std::make_unique<note::test::PopulatedJsonReader>();
    body->set("temperature", 22.5);
    body->set("humidity", int32_t{60});

    auto reader = std::make_unique<note::test::PopulatedJsonReader>();
    reader->set_object("body", std::move(body));

    auto rsp = note::api::NoteGet::Get::Response::parse(std::move(reader));
    auto r = rsp.bodyAs<MacroReadings>();
    REQUIRE(r.temperature == 22.5f);
    REQUIRE(r.humidity == 60);
}

TEST_CASE("parse<T>() with NOTE_FIELDS macro type") {
    auto reader = std::make_unique<note::test::PopulatedJsonReader>();
    reader->set("temperature", 22.5);
    reader->set("humidity", int32_t{60});

    auto r = note::parse<MacroReadings>(*reader);
    REQUIRE(r.temperature == 22.5f);
    REQUIRE(r.humidity == 60);
}

// ── NoteTemplate::Set::Response parsing ─────────────────────────────────────

TEST_CASE("note.template response: template_:true and bytes") {
    auto reader = std::make_unique<note::test::PopulatedJsonReader>();
    reader->set("bytes", int32_t{26});
    reader->set("template", true);
    auto rsp = note::api::NoteTemplate::Set::Response::parse(std::move(reader));
    REQUIRE(rsp.template_ == true);
    REQUIRE(rsp.bytes == 26);
}

TEST_CASE("note.template response: template_:false when no existing template") {
    auto reader = std::make_unique<note::test::PopulatedJsonReader>();
    reader->set("template", false);
    auto rsp = note::api::NoteTemplate::Set::Response::parse(std::move(reader));
    REQUIRE(rsp.template_ == false);
    REQUIRE(rsp.bytes == 0);
}

TEST_CASE("note.template response: body() returns existing template body") {
    auto body = std::make_unique<note::test::PopulatedJsonReader>();
    body->set("temperature", 14.1);
    body->set("humidity", int32_t{11});

    auto reader = std::make_unique<note::test::PopulatedJsonReader>();
    reader->set("bytes", int32_t{26});
    reader->set("template", true);
    reader->set_object("body", std::move(body));

    auto rsp = note::api::NoteTemplate::Set::Response::parse(std::move(reader));
    REQUIRE(rsp.template_ == true);
    REQUIRE(rsp.body() != nullptr);
    REQUIRE(rsp.body()->get_double("temperature") == 14.1);
    REQUIRE(rsp.body()->get_int("humidity") == 11);
}

TEST_CASE("note.template response: body() is null when no body in response") {
    auto reader = std::make_unique<note::test::PopulatedJsonReader>();
    reader->set("bytes", int32_t{14});
    auto rsp = note::api::NoteTemplate::Set::Response::parse(std::move(reader));
    REQUIRE(rsp.body() == nullptr);
}

#if __cplusplus >= 202002L

TEST_CASE("note.template response: bodyAs<T>() parses existing template body") {
    auto body = std::make_unique<note::test::PopulatedJsonReader>();
    body->set("temperature", 22.5);
    body->set("humidity", int32_t{60});

    auto reader = std::make_unique<note::test::PopulatedJsonReader>();
    reader->set("template", true);
    reader->set_object("body", std::move(body));

    auto rsp = note::api::NoteTemplate::Set::Response::parse(std::move(reader));
    auto r = rsp.bodyAs<Readings>();
    REQUIRE(r.temperature == 22.5f);
    REQUIRE(r.humidity == 60);
}

#endif // C++20
