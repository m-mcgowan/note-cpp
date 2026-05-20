// Tests for BodyValue: string, builder, schema, and response body parsing.
#include <doctest.h>
#include "test_json_backend.hpp"
#include "test_notecard_factory.hpp"

#include <note/body.hpp>
#include <note/json_buf.hpp>
#include <note/notecard.hpp>
#include <note/api.hpp>
#include <memory>
#include <map>
#include <deque>
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
    note::test::CallbackTransport transport;
    std::unique_ptr<note::Notecard> nc_ptr;
    note::Notecard& nc;

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
        , nc_ptr(note::test::make_test_notecard_heap(backend, transport))
        , nc(*nc_ptr) {}
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

// ---------------------------------------------------------------------------
// BodyValue from raw JSON string — must be embedded unquoted on the wire.
// The body is a JSON object, not a string value.
// ---------------------------------------------------------------------------

TEST_CASE("BodyValue from raw JSON object string") {
    TestHarness h;
    TestRequest req;
    req.body = R"({"temp":22.5,"humidity":60})";
    h.nc.execute(req);
    REQUIRE(h.last_request ==
        R"({"req":"test.req","body":{"temp":22.5,"humidity":60}})");
}

TEST_CASE("BodyValue from raw JSON with nested object") {
    TestHarness h;
    TestRequest req;
    req.body = R"({"location":{"lat":42.5,"lon":-71.5}})";
    h.nc.execute(req);
    REQUIRE(h.last_request ==
        R"({"req":"test.req","body":{"location":{"lat":42.5,"lon":-71.5}}})");
}

TEST_CASE("BodyValue from raw JSON with string values") {
    TestHarness h;
    TestRequest req;
    req.body = R"({"name":"sensor-1","status":"active"})";
    h.nc.execute(req);
    REQUIRE(h.last_request ==
        R"({"req":"test.req","body":{"name":"sensor-1","status":"active"}})");
}

TEST_CASE("BodyValue from empty object") {
    TestHarness h;
    TestRequest req;
    req.body = "{}";
    h.nc.execute(req);
    REQUIRE(h.last_request ==
        R"({"req":"test.req","body":{}})");
}

#if __cplusplus >= 202002L
TEST_CASE("BodyValue from JsonBuf — compile-time verified") {
    TestHarness h;
    TestRequest req;
    constexpr auto j = note::json<[](auto& b) {
        b.add("temp", 22.5);
        b.add("humidity", 60);
        b.close();
    }>();
    req.body = j.view();
    h.nc.execute(req);
    REQUIRE(h.last_request ==
        R"({"req":"test.req","body":{"temp":22.5,"humidity":60}})");
}
#endif

TEST_CASE("JsonBuf auto-close on view() — no explicit close needed") {
    note::JsonBuf<64> body;
    body.add("temp", 22.5);
    body.add("humidity", 60);
    // No close() — view() auto-closes
    REQUIRE(body.view() == R"({"temp":22.5,"humidity":60})");
}

TEST_CASE("JsonBuf explicit close is idempotent") {
    note::JsonBuf<64> body;
    body.add("temp", 22.5);
    body.close();
    body.close();  // second close is no-op
    REQUIRE(body.view() == R"({"temp":22.5})");
}

TEST_CASE("JsonBuf nested object with auto-close") {
    note::JsonBuf<128> body;
    body.add("temp", 22.5);
    body.begin_object("location");
    body.add("lat", 42.5);
    body.add("lon", -71.5);
    body.end_object();
    // Top-level auto-closed by view()
    REQUIRE(body.view() == R"({"temp":22.5,"location":{"lat":42.5,"lon":-71.5}})");
}

TEST_CASE("JsonBuf as body — runtime values, auto-close") {
    TestHarness h;
    TestRequest req;
    note::JsonBuf<64> body;
    body.add("temp", 22.5);
    body.add("humidity", 60);
    req.body = body.view();
    h.nc.execute(req);
    REQUIRE(h.last_request ==
        R"({"req":"test.req","body":{"temp":22.5,"humidity":60}})");
}

TEST_CASE("JsonBuf conditional fields — body shape varies") {
    TestHarness h;
    TestRequest req;
    note::JsonBuf<128> body;
    body.add("temp", 22.5);
    bool have_gps = true;
    if (have_gps) {
        body.add("lat", 42.5);
        body.add("lon", -71.5);
    }
    req.body = body.view();
    h.nc.execute(req);
    REQUIRE(h.last_request ==
        R"({"req":"test.req","body":{"temp":22.5,"lat":42.5,"lon":-71.5}})");
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
        b.add("humidity", 60);
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
            b.add("lat", 42.5);
            b.add("lon", -71.5);
        b.end_object();
    });
    h.nc.execute(req);
    REQUIRE(h.last_request ==
        R"({"req":"test.req","body":{"temp":22.5,"location":{"lat":42.5,"lon":-71.5}}})");
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
    SensorWithLocation s{.temp = 21.0f, .pos = {.lat = 42.5, .lon = -71.5}};
    req.body = note::make_schema_body(s);
    h.nc.execute(req);
    REQUIRE(h.last_request ==
        R"({"req":"test.req","body":{"temp":21,"pos":{"lat":42.5,"lon":-71.5}}})");
}

TEST_CASE("note.template verify:true includes verify field in request") {
    TestHarness h;
    note::Api api(h.nc);
    api.note.templates().define("sensors.qo")
        .body(note::template_of<Readings>())
        .verify(true)
        .execute();
    // Wire order: required fields (file) emitted before the optional-field
    // table; body sits in the table alongside other optionals so it follows
    // file. JSON key order is not semantically significant.
    REQUIRE(h.last_request ==
        R"({"req":"note.template","file":"sensors.qo","body":{"temperature":14.1,"humidity":11},"verify":true})");
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

TEST_CASE("note.add with string body — embedded unquoted") {
    TestHarness h;
    note::Api api(h.nc);
    auto req = api.note.add();
    req.file = "sensors.qo";
    req.body = R"({"temp":22.5})";
    req.execute();
    REQUIRE(h.last_request ==
        R"({"req":"note.add","body":{"temp":22.5},"file":"sensors.qo"})");
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
        R"({"req":"note.template","file":"sensors.qo","body":{"temperature":14.1,"humidity":11}})");
}

TEST_CASE("note.add with builder body") {
    TestHarness h;
    note::Api api(h.nc);
    api.note.add()
        .file("sensors.qo")
        .body(note::body([](note::JsonBuilder& b) {
            b.add("temp", 22.5);
            b.add("count", 42);
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
    REQUIRE_FALSE(rsp.was_streaming_parse());
    // body_or_error() success path mirrors body() in the buffered case.
    auto safe = rsp.body_or_error();
    REQUIRE(safe.has_value());
    REQUIRE(*safe != nullptr);
}

TEST_CASE("note.get streaming-parsed response: body_or_error reports streaming mode") {
    // Streaming Sink construction marks the Response as streaming-parsed.
    // Even when the Response is otherwise default-initialised, body_or_error
    // must surface an explicit Error::NotReady so callers can react instead
    // of silently dereferencing a null body() pointer.
    using Rsp = note::api::NoteGet::Get::Response;
    Rsp rsp;
    char arena_buf[256];
    note::MonotonicArena arena(arena_buf);
    note::StringPool pool(note::arena_allocator(arena));
    Rsp::Sink sink(rsp, pool);
    // No sink.reset() — the flag must be set by the Sink constructor itself,
    // so successful first-attempt executes (which never call reset()) still
    // report streaming mode.
    REQUIRE(rsp.was_streaming_parse());
    REQUIRE(rsp.body() == nullptr);
    auto safe = rsp.body_or_error();
    REQUIRE_FALSE(safe.has_value());
    CHECK(safe.error().code == note::Error::NotReady);
#if !NOTE_SHORT_ERRORS
    auto msg = note::string_view{safe.error().message};
    CHECK_MESSAGE(msg.find("streaming mode") != note::string_view::npos,
                  "should name streaming mode; got: ", std::string{msg});
    CHECK_MESSAGE(msg.find(".into(") != note::string_view::npos,
                  "should name .into(...) alternative; got: ", std::string{msg});
#endif
}

// End-to-end coverage of the same invariant — drives the streaming path
// through Notecard::execute() so a successful first attempt (no retry)
// has to set the flag on its own. Catches the gap where Sink::reset()
// was the only writer of streaming_parse_used_.
TEST_CASE("note.get via streaming execute: was_streaming_parse + body_or_error") {
    struct StreamingHal : note::Hal {
        std::deque<uint8_t> rx;
        void queue(const std::string& s) {
            for (char c : s) rx.push_back(static_cast<uint8_t>(c));
            rx.push_back('\n');
        }
        bool transmit(const uint8_t*, size_t) override { return true; }
        bool write_line_terminator() override { return true; }
        note::Result<size_t> read(uint8_t* buf, size_t max_len, uint32_t) override {
            if (rx.empty())
                return note::make_error(note::Error::ResponseLost, note::Cause::Timeout, "no data");
            size_t n = std::min(max_len, rx.size());
            for (size_t i = 0; i < n; ++i) { buf[i] = rx.front(); rx.pop_front(); }
            return n;
        }
        bool reset() override { return true; }
        void delay(uint32_t) override {}
        uint32_t millis() override { return 0; }
    };

    StreamingHal hal;
    hal.queue(R"({"body":{"temperature":22.5,"humidity":60},"time":1700000000})");
    note::Protocol transport{hal};
    auto nc_ptr = note::test::make_test_notecard_heap(transport, note::Allocator{});
    auto& nc = *nc_ptr;
    note::Api api(nc);

    auto r = api.note.read("sensors.qi").execute();
    REQUIRE(r.has_value());
    CHECK(r.was_streaming_parse());
    CHECK(r.body() == nullptr);

    auto safe = r.body_or_error();
    REQUIRE_FALSE(safe.has_value());
    CHECK(safe.error().code == note::Error::NotReady);
}

#if __cplusplus >= 202002L

TEST_CASE("note.get response parse<T>(body) with reflected struct") {
    auto body = std::make_unique<note::test::PopulatedJsonReader>();
    body->set("temperature", 22.5);
    body->set("humidity", int32_t{60});

    auto reader = std::make_unique<note::test::PopulatedJsonReader>();
    reader->set_object("body", std::move(body));

    auto rsp = note::api::NoteGet::Get::Response::parse(std::move(reader));
    REQUIRE(rsp.body() != nullptr);
    auto r = note::parse<Readings>(*rsp.body());
    REQUIRE(r.temperature == 22.5f);
    REQUIRE(r.humidity == 60);
}

TEST_CASE("note.get response body() is null when no body") {
    auto reader = std::make_unique<note::test::PopulatedJsonReader>();
    auto rsp = note::api::NoteGet::Get::Response::parse(std::move(reader));
    REQUIRE(rsp.body() == nullptr);
}

TEST_CASE("note.get response parse<T>(body) with mixed types") {
    auto body = std::make_unique<note::test::PopulatedJsonReader>();
    body->set("voltage", 3.3);
    body->set("active", true);
    body->set("count", int32_t{42});

    auto reader = std::make_unique<note::test::PopulatedJsonReader>();
    reader->set_object("body", std::move(body));

    auto rsp = note::api::NoteGet::Get::Response::parse(std::move(reader));
    auto sd = note::parse<SensorData>(*rsp.body());
    REQUIRE(sd.voltage == 3.3);
    REQUIRE(sd.active == true);
    REQUIRE(sd.count == 42);
}

#endif // C++20

// ── NOTE_FIELDS macro response parsing ───────────────────────────────────────

TEST_CASE("note.get response parse<T>(body) with NOTE_FIELDS macro type") {
    auto body = std::make_unique<note::test::PopulatedJsonReader>();
    body->set("temperature", 22.5);
    body->set("humidity", int32_t{60});

    auto reader = std::make_unique<note::test::PopulatedJsonReader>();
    reader->set_object("body", std::move(body));

    auto rsp = note::api::NoteGet::Get::Response::parse(std::move(reader));
    REQUIRE(rsp.body() != nullptr);
    auto r = note::parse<MacroReadings>(*rsp.body());
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

TEST_CASE("note.template response: parse<T>(body) parses existing template body") {
    auto body = std::make_unique<note::test::PopulatedJsonReader>();
    body->set("temperature", 22.5);
    body->set("humidity", int32_t{60});

    auto reader = std::make_unique<note::test::PopulatedJsonReader>();
    reader->set("template", true);
    reader->set_object("body", std::move(body));

    auto rsp = note::api::NoteTemplate::Set::Response::parse(std::move(reader));
    REQUIRE(rsp.body() != nullptr);
    auto r = note::parse<Readings>(*rsp.body());
    REQUIRE(r.temperature == 22.5f);
    REQUIRE(r.humidity == 60);
}

#endif // C++20
