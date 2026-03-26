// Tests for BufferJsonBackend, BufferJsonBuilder, and JsmnJsonReader.
// Exercises the zero-allocation JSON backend used on embedded targets.

#include "catch.hpp"

#include <note/backends/buffer.hpp>

#include <cstring>
#include <string>

using namespace note;
using namespace note::backends;

// ---------------------------------------------------------------------------
// BufferJsonBuilder
// ---------------------------------------------------------------------------

TEST_CASE("BufferJsonBuilder: basic key-value types") {
    char buf[256];
    BufferJsonBuilder b(buf, sizeof(buf));

    b.add("flag", true);
    b.add("count", int32_t{42});
    b.add("temp", 22.5);
    b.add("name", string_view("hello"));

    REQUIRE(b.to_view() == R"({"flag":true,"count":42,"temp":22.5,"name":"hello"})");
}

TEST_CASE("BufferJsonBuilder: false boolean") {
    char buf[128];
    BufferJsonBuilder b(buf, sizeof(buf));
    b.add("off", false);
    REQUIRE(b.to_view() == R"({"off":false})");
}

TEST_CASE("BufferJsonBuilder: negative integer") {
    char buf[128];
    BufferJsonBuilder b(buf, sizeof(buf));
    b.add("val", int32_t{-100});
    REQUIRE(b.to_view() == R"({"val":-100})");
}

TEST_CASE("BufferJsonBuilder: nested object") {
    char buf[256];
    BufferJsonBuilder b(buf, sizeof(buf));
    b.add("req", string_view("note.add"));
    b.begin_object("body");
    b.add("temp", 22.5);
    b.add("label", string_view("room"));
    b.end_object();
    REQUIRE(b.to_view() == R"({"req":"note.add","body":{"temp":22.5,"label":"room"}})");
}

TEST_CASE("BufferJsonBuilder: array with elements") {
    char buf[256];
    BufferJsonBuilder b(buf, sizeof(buf));
    b.begin_array("files");
    b.add_element(string_view("data.qi"));
    b.add_element(string_view("settings.db"));
    b.end_array();
    REQUIRE(b.to_view() == R"({"files":["data.qi","settings.db"]})");
}

TEST_CASE("BufferJsonBuilder: array with mixed types") {
    char buf[256];
    BufferJsonBuilder b(buf, sizeof(buf));
    b.begin_array("items");
    b.add_element(true);
    b.add_element(int32_t{7});
    b.add_element(3.14);
    b.add_element(string_view("text"));
    b.end_array();
    REQUIRE(b.to_view() == R"({"items":[true,7,3.14,"text"]})");
}

TEST_CASE("BufferJsonBuilder: string escaping") {
    char buf[256];
    BufferJsonBuilder b(buf, sizeof(buf));
    b.add("msg", string_view("line1\nline2\ttab\"quote\\backslash"));
    auto json = b.to_view();
    REQUIRE(json.find("\\n") != std::string::npos);
    REQUIRE(json.find("\\t") != std::string::npos);
    REQUIRE(json.find("\\\"") != std::string::npos);
    REQUIRE(json.find("\\\\") != std::string::npos);
}

TEST_CASE("BufferJsonBuilder: reset reuses buffer") {
    char buf[128];
    BufferJsonBuilder b(buf, sizeof(buf));
    b.add("first", true);
    REQUIRE(b.to_view() == R"({"first":true})");

    b.reset();
    b.add("second", false);
    REQUIRE(b.to_view() == R"({"second":false})");
}

TEST_CASE("BufferJsonBuilder: to_string returns std::string") {
    char buf[128];
    BufferJsonBuilder b(buf, sizeof(buf));
    b.add("x", int32_t{1});
    std::string s = b.to_string();
    REQUIRE(s == R"({"x":1})");
}

TEST_CASE("BufferJsonBuilder: overflow truncates gracefully") {
    char buf[16];  // Very small buffer
    BufferJsonBuilder b(buf, sizeof(buf));
    b.add("longkey", string_view("longvalue"));
    REQUIRE(b.overflow());
    // Should not crash, output is truncated
    auto v = b.to_view();
    REQUIRE(v.size() <= sizeof(buf));
}

TEST_CASE("BufferJsonBuilder: empty object") {
    char buf[64];
    BufferJsonBuilder b(buf, sizeof(buf));
    REQUIRE(b.to_view() == "{}");
}

// ---------------------------------------------------------------------------
// JsmnJsonReader
// ---------------------------------------------------------------------------

TEST_CASE("JsmnJsonReader: read all types") {
    const char* json = R"({"flag":true,"count":42,"temp":22.5,"name":"hello"})";
    jsmntok_t tokens[16];
    JsmnJsonReader r(json, strlen(json), tokens, 16);

    REQUIRE_FALSE(r.has_error());
    REQUIRE(r.get_bool("flag", false) == true);
    REQUIRE(r.get_int("count", 0) == 42);
    REQUIRE(r.get_double("temp", 0.0) == Approx(22.5));
    REQUIRE(r.get_string("name") == "hello");
}

TEST_CASE("JsmnJsonReader: missing keys return defaults") {
    const char* json = R"({"x":1})";
    jsmntok_t tokens[8];
    JsmnJsonReader r(json, strlen(json), tokens, 8);

    REQUIRE(r.get_bool("missing", true) == true);
    REQUIRE(r.get_int("missing", -1) == -1);
    REQUIRE(r.get_double("missing", 9.9) == Approx(9.9));
    REQUIRE(r.get_string("missing", "def") == "def");
}

TEST_CASE("JsmnJsonReader: has() checks key presence") {
    const char* json = R"({"present":"yes"})";
    jsmntok_t tokens[8];
    JsmnJsonReader r(json, strlen(json), tokens, 8);

    REQUIRE(r.has("present"));
    REQUIRE_FALSE(r.has("absent"));
}

TEST_CASE("JsmnJsonReader: false boolean") {
    const char* json = R"({"off":false})";
    jsmntok_t tokens[8];
    JsmnJsonReader r(json, strlen(json), tokens, 8);
    REQUIRE(r.get_bool("off", true) == false);
}

TEST_CASE("JsmnJsonReader: negative integer") {
    const char* json = R"({"val":-100})";
    jsmntok_t tokens[8];
    JsmnJsonReader r(json, strlen(json), tokens, 8);
    REQUIRE(r.get_int("val", 0) == -100);
}

TEST_CASE("JsmnJsonReader: integer from decimal truncates") {
    const char* json = R"({"val":3.7})";
    jsmntok_t tokens[8];
    JsmnJsonReader r(json, strlen(json), tokens, 8);
    REQUIRE(r.get_int("val", 0) == 3);
}

TEST_CASE("JsmnJsonReader: nested object") {
    const char* json = R"({"body":{"temp":22.5,"label":"room"},"err":""})";
    jsmntok_t tokens[16];
    JsmnJsonReader r(json, strlen(json), tokens, 16);

    auto body = r.get_object("body");
    REQUIRE(body);
    REQUIRE(body->get_double("temp", 0.0) == Approx(22.5));
    REQUIRE(body->get_string("label") == "room");
}

TEST_CASE("JsmnJsonReader: get_object returns nullptr for missing key") {
    const char* json = R"({"x":1})";
    jsmntok_t tokens[8];
    JsmnJsonReader r(json, strlen(json), tokens, 8);
    REQUIRE(r.get_object("body") == nullptr);
}

TEST_CASE("JsmnJsonReader: get_object returns nullptr for non-object") {
    const char* json = R"({"body":"string"})";
    jsmntok_t tokens[8];
    JsmnJsonReader r(json, strlen(json), tokens, 8);
    REQUIRE(r.get_object("body") == nullptr);
}

TEST_CASE("JsmnJsonReader: get_string returns default for non-string") {
    const char* json = R"({"val":42})";
    jsmntok_t tokens[8];
    JsmnJsonReader r(json, strlen(json), tokens, 8);
    REQUIRE(r.get_string("val", "def") == "def");
}

TEST_CASE("JsmnJsonReader: error detection") {
    SECTION("invalid JSON") {
        const char* json = "not json at all";
        jsmntok_t tokens[8];
        JsmnJsonReader r(json, strlen(json), tokens, 8);
        REQUIRE(r.has_error());
        REQUIRE(r.get_error() == "JSON parse error");
    }

    SECTION("empty input") {
        jsmntok_t tokens[8];
        JsmnJsonReader r("", 0, tokens, 8);
        REQUIRE(r.has_error());
    }

    SECTION("notecard error field") {
        const char* json = R"({"err":"something went wrong"})";
        jsmntok_t tokens[8];
        JsmnJsonReader r(json, strlen(json), tokens, 8);
        REQUIRE_FALSE(r.has_error());
        REQUIRE(r.get_error() == "something went wrong");
    }

    SECTION("no error field returns empty") {
        const char* json = R"({"ok":true})";
        jsmntok_t tokens[8];
        JsmnJsonReader r(json, strlen(json), tokens, 8);
        REQUIRE(r.get_error().empty());
    }
}

TEST_CASE("JsmnJsonReader: object after nested object") {
    // Verify that keys after a nested object are found correctly (tok_span)
    const char* json = R"({"body":{"a":1},"after":"found"})";
    jsmntok_t tokens[16];
    JsmnJsonReader r(json, strlen(json), tokens, 16);

    REQUIRE(r.has("body"));
    REQUIRE(r.has("after"));
    REQUIRE(r.get_string("after") == "found");
}

TEST_CASE("JsmnJsonReader: object after nested array") {
    const char* json = R"({"arr":[1,2,3],"after":"found"})";
    jsmntok_t tokens[16];
    JsmnJsonReader r(json, strlen(json), tokens, 16);

    REQUIRE(r.has("arr"));
    REQUIRE(r.has("after"));
    REQUIRE(r.get_string("after") == "found");
}

// ---------------------------------------------------------------------------
// BufferJsonBackend integration
// ---------------------------------------------------------------------------

TEST_CASE("BufferJsonBackend: build and parse round-trip") {
    BufferJsonBackend<256, 32> backend;

    auto& builder = backend.get_builder();
    builder.add("req", string_view("card.version"));
    auto json = builder.to_view();
    REQUIRE(json == R"({"req":"card.version"})");

    // Parse the response
    const char* response = R"({"version":"notecard-7.2.1","device":"dev:123"})";
    auto& reader = backend.get_reader(string_view(response, strlen(response)));
    REQUIRE_FALSE(reader.has_error());
    REQUIRE(reader.get_string("version") == "notecard-7.2.1");
    REQUIRE(reader.get_string("device") == "dev:123");
}

TEST_CASE("BufferJsonBackend: get_builder resets between calls") {
    BufferJsonBackend<256, 32> backend;

    auto& b1 = backend.get_builder();
    b1.add("first", true);
    REQUIRE(b1.to_view() == R"({"first":true})");

    auto& b2 = backend.get_builder();
    b2.add("second", false);
    REQUIRE(b2.to_view() == R"({"second":false})");
}

TEST_CASE("BufferJsonBackend: parse_response returns owned reader") {
    BufferJsonBackend<256, 32> backend;

    const char* json = R"({"val":42})";
    auto reader = backend.parse_response(string_view(json, strlen(json)));
    REQUIRE(reader);
    REQUIRE(reader->get_int("val", 0) == 42);
}

TEST_CASE("BufferJsonBackend: create_builder returns owned builder") {
    BufferJsonBackend<256, 32> backend;

    auto builder = backend.create_builder();
    builder->add("key", string_view("val"));
    REQUIRE(builder->to_view() == R"({"key":"val"})");
}
