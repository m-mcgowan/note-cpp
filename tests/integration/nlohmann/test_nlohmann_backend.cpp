// Integration test for the nlohmann-json backend.
// Verifies request building, response parsing, nested objects, and error handling.

#include <note/backends/nlohmann.hpp>

#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>

using namespace note::backends;

// ---------------------------------------------------------------------------
// Builder tests
// ---------------------------------------------------------------------------
static void test_builder_simple() {
    NlohmannBackend backend;
    auto builder = backend.create_builder();
    builder->add("req", "hub.set");
    builder->add("product", "com.example.app");
    builder->add("mode", "periodic");
    builder->add("outbound", int32_t{60});
    auto json = builder->to_string();

    assert(json.find("\"req\":\"hub.set\"") != std::string::npos);
    assert(json.find("\"product\":\"com.example.app\"") != std::string::npos);
    assert(json.find("\"outbound\":60") != std::string::npos);
    std::puts("  PASS: builder_simple");
}

static void test_builder_types() {
    NlohmannBackend backend;
    auto builder = backend.create_builder();
    builder->add("flag", true);
    builder->add("count", int32_t{42});
    builder->add("value", 3.14);
    builder->add("name", "test");
    auto json = builder->to_string();

    assert(json.find("\"flag\":true") != std::string::npos);
    assert(json.find("\"count\":42") != std::string::npos);
    assert(json.find("\"name\":\"test\"") != std::string::npos);
    std::puts("  PASS: builder_types");
}

static void test_builder_nested_object() {
    NlohmannBackend backend;
    auto builder = backend.create_builder();
    builder->add("req", "note.add");
    builder->begin_object("body");
    builder->add("temp", 22.5);
    builder->add("humidity", int32_t{60});
    builder->end_object();
    auto json = builder->to_string();

    assert(json.find("\"body\":{") != std::string::npos);
    assert(json.find("\"temp\":22.5") != std::string::npos);
    assert(json.find("\"humidity\":60") != std::string::npos);
    std::puts("  PASS: builder_nested_object");
}

// ---------------------------------------------------------------------------
// Reader tests
// ---------------------------------------------------------------------------
static void test_reader_simple() {
    NlohmannBackend backend;
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
    NlohmannBackend backend;
    auto reader = backend.parse_response(R"({"int_val":42,"float_val":3.14,"neg":-7})");

    assert(reader->get_int("int_val") == 42);
    assert(std::abs(reader->get_double("float_val") - 3.14) < 0.001);
    assert(reader->get_int("neg") == -7);
    std::puts("  PASS: reader_numbers");
}

static void test_reader_nested_object() {
    NlohmannBackend backend;
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
    NlohmannBackend backend;

    // Notecard error response
    auto reader = backend.parse_response(R"({"err":"file not found"})");
    assert(reader->get_error() == "file not found");

    // Invalid JSON — nlohmann::json::parse with allow_exceptions=false returns null
    auto bad = backend.parse_response("not json");
    assert(bad->has_error());
    std::puts("  PASS: reader_error");
}

static void test_reader_defaults() {
    NlohmannBackend backend;
    auto reader = backend.parse_response("{}");

    assert(reader->get_bool("x", true) == true);
    assert(reader->get_int("x", 99) == 99);
    assert(std::abs(reader->get_double("x", 1.5) - 1.5) < 0.001);
    assert(reader->get_string("x", "default") == "default");
    std::puts("  PASS: reader_defaults");
}

// ---------------------------------------------------------------------------
// Round-trip test
// ---------------------------------------------------------------------------
static void test_round_trip() {
    NlohmannBackend backend;

    // Build a request
    auto builder = backend.create_builder();
    builder->add("req", "note.add");
    builder->add("file", "sensors.qo");
    builder->begin_object("body");
    builder->add("temp", 22.5);
    builder->add("humidity", int32_t{60});
    builder->end_object();
    auto json = builder->to_string();

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
int main() {
    std::puts("=== nlohmann-json backend integration tests ===");
    test_builder_simple();
    test_builder_types();
    test_builder_nested_object();
    test_reader_simple();
    test_reader_numbers();
    test_reader_nested_object();
    test_reader_error();
    test_reader_defaults();
    test_round_trip();
    std::puts("\nAll nlohmann-json backend tests passed.");
    return 0;
}
