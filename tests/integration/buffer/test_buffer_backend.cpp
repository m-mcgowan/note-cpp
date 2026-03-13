// Integration test for the buffer JSON backend (BufferJsonBuilder + JsmnJsonReader).
// Same test structure as test_cjson_backend.cpp to verify identical behavior.

#include <note/backends/buffer.hpp>

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

using namespace note::backends;

// ---------------------------------------------------------------------------
// Builder tests
// ---------------------------------------------------------------------------
static void test_builder_simple() {
    BufferJsonBackend<> backend;
    auto& builder = backend.get_builder();
    builder.add("req", "hub.set");
    builder.add("product", "com.example.app");
    builder.add("mode", "periodic");
    builder.add("outbound", int32_t{60});
    auto json = builder.to_view();

    assert(json.find("\"req\":\"hub.set\"") != std::string::npos);
    assert(json.find("\"product\":\"com.example.app\"") != std::string::npos);
    assert(json.find("\"outbound\":60") != std::string::npos);
    std::puts("  PASS: builder_simple");
}

static void test_builder_types() {
    BufferJsonBackend<> backend;
    auto& builder = backend.get_builder();
    builder.add("flag", true);
    builder.add("count", int32_t{42});
    builder.add("value", 3.14);
    builder.add("name", "test");
    auto json = builder.to_view();

    assert(json.find("\"flag\":true") != std::string::npos);
    assert(json.find("\"count\":42") != std::string::npos);
    assert(json.find("\"value\":3.14") != std::string::npos);
    assert(json.find("\"name\":\"test\"") != std::string::npos);
    std::puts("  PASS: builder_types");
}

static void test_builder_nested_object() {
    BufferJsonBackend<> backend;
    auto& builder = backend.get_builder();
    builder.add("req", "note.add");
    builder.begin_object("body");
    builder.add("temp", 22.5);
    builder.add("humidity", int32_t{60});
    builder.end_object();
    auto json = builder.to_view();

    assert(json.find("\"body\":{") != std::string::npos);
    assert(json.find("\"temp\":22.5") != std::string::npos);
    assert(json.find("\"humidity\":60") != std::string::npos);
    std::puts("  PASS: builder_nested_object");
}

static void test_builder_reset() {
    BufferJsonBackend<> backend;
    auto& builder = backend.get_builder();
    builder.add("req", "first");
    auto json1 = std::string(builder.to_view());

    // Reset via get_builder
    auto& builder2 = backend.get_builder();
    builder2.add("req", "second");
    auto json2 = builder2.to_view();

    assert(json1.find("\"first\"") != std::string::npos);
    assert(json2.find("\"second\"") != std::string::npos);
    assert(json2.find("\"first\"") == std::string::npos);
    std::puts("  PASS: builder_reset");
}

static void test_builder_to_string() {
    BufferJsonBackend<> backend;
    auto& builder = backend.get_builder();
    builder.add("req", "test");
    auto s = builder.to_string();
    assert(s == "{\"req\":\"test\"}");
    std::puts("  PASS: builder_to_string");
}

// ---------------------------------------------------------------------------
// Reader tests
// ---------------------------------------------------------------------------
static void test_reader_simple() {
    BufferJsonBackend<> backend;
    auto reader = backend.parse_response(R"({"version":"notecard-7.2.1","device":"dev:1234","connected":true,"cells":3})");

    assert(reader->has("version"));
    assert(reader->get_string("version") == "notecard-7.2.1");
    assert(reader->get_string("device") == "dev:1234");
    assert(reader->get_bool("connected") == true);
    assert(reader->get_int("cells") == 3);
    assert(!reader->has("missing"));
    assert(reader->get_string("missing", "fallback") == "fallback");
    std::puts("  PASS: reader_simple");
}

static void test_reader_numbers() {
    BufferJsonBackend<> backend;
    auto reader = backend.parse_response(R"({"int_val":42,"float_val":3.14,"neg":-7})");

    assert(reader->get_int("int_val") == 42);
    assert(std::abs(reader->get_double("float_val") - 3.14) < 0.001);
    assert(reader->get_int("neg") == -7);
    std::puts("  PASS: reader_numbers");
}

static void test_reader_nested_object() {
    BufferJsonBackend<> backend;
    auto reader = backend.parse_response(R"({"body":{"temp":22.5,"label":"room-1"},"file":"data.qi"})");

    assert(reader->get_string("file") == "data.qi");

    auto body = reader->get_object("body");
    assert(body != nullptr);
    assert(std::abs(body->get_double("temp") - 22.5) < 0.001);
    assert(body->get_string("label") == "room-1");

    // Non-existent object returns nullptr
    assert(reader->get_object("missing") == nullptr);
    std::puts("  PASS: reader_nested_object");
}

static void test_reader_error() {
    BufferJsonBackend<> backend;

    // Notecard error response
    auto reader = backend.parse_response(R"({"err":"file not found"})");
    assert(reader->get_error() == "file not found");

    // Invalid JSON
    auto bad = backend.parse_response("not json");
    assert(bad->has_error());
    std::puts("  PASS: reader_error");
}

static void test_reader_defaults() {
    BufferJsonBackend<> backend;
    auto reader = backend.parse_response("{}");

    assert(reader->get_bool("x", true) == true);
    assert(reader->get_int("x", 99) == 99);
    assert(std::abs(reader->get_double("x", 1.5) - 1.5) < 0.001);
    assert(reader->get_string("x", "default") == "default");
    std::puts("  PASS: reader_defaults");
}

static void test_reader_bool_false() {
    BufferJsonBackend<> backend;
    auto reader = backend.parse_response(R"({"a":false,"b":true})");
    assert(reader->get_bool("a") == false);
    assert(reader->get_bool("b") == true);
    std::puts("  PASS: reader_bool_false");
}

// ---------------------------------------------------------------------------
// Round-trip test
// ---------------------------------------------------------------------------
static void test_round_trip() {
    BufferJsonBackend<> backend;

    // Build a request
    auto& builder = backend.get_builder();
    builder.add("req", "note.add");
    builder.add("file", "sensors.qo");
    builder.begin_object("body");
    builder.add("temp", 22.5);
    builder.add("humidity", int32_t{60});
    builder.end_object();
    auto json = std::string(builder.to_view());

    // Parse it back
    auto reader = backend.parse_response(json);
    assert(reader->get_string("req") == "note.add");
    assert(reader->get_string("file") == "sensors.qo");

    auto body = reader->get_object("body");
    assert(body != nullptr);
    assert(std::abs(body->get_double("temp") - 22.5) < 0.001);
    assert(body->get_int("humidity") == 60);
    std::puts("  PASS: round_trip");
}

// ---------------------------------------------------------------------------
// Builder escape test
// ---------------------------------------------------------------------------
static void test_builder_escape() {
    BufferJsonBackend<> backend;
    auto& builder = backend.get_builder();
    builder.add("msg", "hello \"world\"\nnewline");
    auto json = builder.to_view();
    assert(json.find("\\\"world\\\"") != std::string::npos);
    assert(json.find("\\n") != std::string::npos);
    std::puts("  PASS: builder_escape");
}

// ---------------------------------------------------------------------------
// Array support test
// ---------------------------------------------------------------------------
static void test_builder_array() {
    BufferJsonBackend<512, 128> backend;
    auto& builder = backend.get_builder();
    builder.add("req", "test");
    builder.begin_array("tags");
    // Array elements need to be added via the builder — but our interface
    // only supports keyed add. The array is still opened/closed correctly.
    builder.end_array();
    auto json = builder.to_view();
    assert(json.find("\"tags\":[]") != std::string::npos);
    std::puts("  PASS: builder_array");
}

// ---------------------------------------------------------------------------
int main() {
    std::puts("=== Buffer backend integration tests ===");
    test_builder_simple();
    test_builder_types();
    test_builder_nested_object();
    test_builder_reset();
    test_builder_to_string();
    test_reader_simple();
    test_reader_numbers();
    test_reader_nested_object();
    test_reader_error();
    test_reader_defaults();
    test_reader_bool_false();
    test_round_trip();
    test_builder_escape();
    test_builder_array();
    std::puts("\nAll buffer backend tests passed.");
    return 0;
}
