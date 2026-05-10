// Integration test for the buffer JSON backend (StaticJsonBuilder + JsmnJsonReader).
// Same test structure as test_cjson_backend.cpp to verify identical behavior.
//
// This file compiles into two binaries:
//   - host: `note-cpp-integration-backends` (tests/CMakeLists.txt)
//   - device: tests/integration/firmware (via symlink in firmware/test/)
// doctest's main comes from tests/doctest_main.cpp (host) or
// tests/integration/firmware/test/main.cpp (device).

#include <doctest.h>

#include <note/backends/buffer.hpp>

#include <cmath>
#include <string>

using namespace note::backends;

TEST_CASE("buffer/builder/simple") {
    StaticJsonBackend<> backend;
    auto& builder = backend.get_builder();
    builder.add("req", "hub.set");
    builder.add("product", "com.example.app");
    builder.add("mode", "periodic");
    builder.add("outbound", int32_t{60});
    auto json = builder.to_view();

    CHECK(json.find("\"req\":\"hub.set\"") != std::string::npos);
    CHECK(json.find("\"product\":\"com.example.app\"") != std::string::npos);
    CHECK(json.find("\"outbound\":60") != std::string::npos);
}

TEST_CASE("buffer/builder/types") {
    StaticJsonBackend<> backend;
    auto& builder = backend.get_builder();
    builder.add("flag", true);
    builder.add("count", int32_t{42});
    builder.add("value", 3.14);
    builder.add("name", "test");
    auto json = builder.to_view();

    CHECK(json.find("\"flag\":true") != std::string::npos);
    CHECK(json.find("\"count\":42") != std::string::npos);
    CHECK(json.find("\"value\":3.14") != std::string::npos);
    CHECK(json.find("\"name\":\"test\"") != std::string::npos);
}

TEST_CASE("buffer/builder/nested_object") {
    StaticJsonBackend<> backend;
    auto& builder = backend.get_builder();
    builder.add("req", "note.add");
    builder.begin_object("body");
    builder.add("temp", 22.5);
    builder.add("humidity", int32_t{60});
    builder.end_object();
    auto json = builder.to_view();

    CHECK(json.find("\"body\":{") != std::string::npos);
    CHECK(json.find("\"temp\":22.5") != std::string::npos);
    CHECK(json.find("\"humidity\":60") != std::string::npos);
}

TEST_CASE("buffer/builder/reset") {
    StaticJsonBackend<> backend;
    auto& builder = backend.get_builder();
    builder.add("req", "first");
    auto json1 = std::string(builder.to_view());

    auto& builder2 = backend.get_builder();
    builder2.add("req", "second");
    auto json2 = builder2.to_view();

    CHECK(json1.find("\"first\"") != std::string::npos);
    CHECK(json2.find("\"second\"") != std::string::npos);
    CHECK(json2.find("\"first\"") == std::string::npos);
}

TEST_CASE("buffer/builder/exact_serialization") {
    StaticJsonBackend<> backend;
    auto& builder = backend.get_builder();
    builder.add("req", "test");
    auto s = std::string(builder.to_view());
    CHECK(s == "{\"req\":\"test\"}");
}

TEST_CASE("buffer/reader/simple") {
    StaticJsonBackend<> backend;
    auto reader = backend.parse_response(
        R"({"version":"notecard-7.2.1","device":"dev:1234","connected":true,"cells":3})");

    CHECK(reader->has("version"));
    CHECK(reader->get_string("version") == "notecard-7.2.1");
    CHECK(reader->get_string("device") == "dev:1234");
    CHECK(reader->get_bool("connected") == true);
    CHECK(reader->get_int("cells") == 3);
    CHECK(!reader->has("missing"));
    CHECK(reader->get_string("missing", "fallback") == "fallback");
}

TEST_CASE("buffer/reader/numbers") {
    StaticJsonBackend<> backend;
    auto reader = backend.parse_response(R"({"int_val":42,"float_val":3.14,"neg":-7})");

    CHECK(reader->get_int("int_val") == 42);
    CHECK(std::abs(reader->get_double("float_val") - 3.14) < 0.001);
    CHECK(reader->get_int("neg") == -7);
}

TEST_CASE("buffer/reader/nested_object") {
    StaticJsonBackend<> backend;
    auto reader = backend.parse_response(
        R"({"body":{"temp":22.5,"label":"room-1"},"file":"data.qi"})");

    CHECK(reader->get_string("file") == "data.qi");

    auto body = reader->get_object("body");
    REQUIRE(body != nullptr);
    CHECK(std::abs(body->get_double("temp") - 22.5) < 0.001);
    CHECK(body->get_string("label") == "room-1");

    CHECK(reader->get_object("missing") == nullptr);
}

TEST_CASE("buffer/reader/error") {
    StaticJsonBackend<> backend;

    auto reader = backend.parse_response(R"({"err":"file not found"})");
    CHECK(reader->get_error() == "file not found");

    auto bad = backend.parse_response("not json");
    CHECK(bad->has_error());
}

TEST_CASE("buffer/reader/defaults") {
    StaticJsonBackend<> backend;
    auto reader = backend.parse_response("{}");

    CHECK(reader->get_bool("x", true) == true);
    CHECK(reader->get_int("x", 99) == 99);
    CHECK(std::abs(reader->get_double("x", 1.5) - 1.5) < 0.001);
    CHECK(reader->get_string("x", "default") == "default");
}

TEST_CASE("buffer/reader/bool_false") {
    StaticJsonBackend<> backend;
    auto reader = backend.parse_response(R"({"a":false,"b":true})");
    CHECK(reader->get_bool("a") == false);
    CHECK(reader->get_bool("b") == true);
}

TEST_CASE("buffer/round_trip") {
    StaticJsonBackend<> backend;

    auto& builder = backend.get_builder();
    builder.add("req", "note.add");
    builder.add("file", "sensors.qo");
    builder.begin_object("body");
    builder.add("temp", 22.5);
    builder.add("humidity", int32_t{60});
    builder.end_object();
    auto json = std::string(builder.to_view());

    auto reader = backend.parse_response(json);
    CHECK(reader->get_string("req") == "note.add");
    CHECK(reader->get_string("file") == "sensors.qo");

    auto body = reader->get_object("body");
    REQUIRE(body != nullptr);
    CHECK(std::abs(body->get_double("temp") - 22.5) < 0.001);
    CHECK(body->get_int("humidity") == 60);
}

TEST_CASE("buffer/builder/escape") {
    StaticJsonBackend<> backend;
    auto& builder = backend.get_builder();
    builder.add("msg", "hello \"world\"\nnewline");
    auto json = builder.to_view();
    CHECK(json.find("\\\"world\\\"") != std::string::npos);
    CHECK(json.find("\\n") != std::string::npos);
}

TEST_CASE("buffer/builder/array") {
    StaticJsonBackend<512, 128> backend;
    auto& builder = backend.get_builder();
    builder.add("req", "test");
    builder.begin_array("tags");
    builder.end_array();
    auto json = builder.to_view();
    CHECK(json.find("\"tags\":[]") != std::string::npos);
}
