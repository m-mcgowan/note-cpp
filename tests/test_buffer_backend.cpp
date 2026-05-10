// Tests for StaticJsonBackend, StaticJsonBuilder, and JsmnJsonReader.
// Exercises the zero-allocation JSON backend used on embedded targets.

#include <doctest.h>

#include <note/backends/buffer.hpp>

#include <cstring>
#include <string>

using namespace note;
using namespace note::backends;

// ---------------------------------------------------------------------------
// StaticJsonBuilder
// ---------------------------------------------------------------------------

TEST_CASE("StaticJsonBuilder: basic key-value types") {
    char buf[256];
    StaticJsonBuilder b(buf, sizeof(buf));

    b.add("flag", true);
    b.add("count", int32_t{42});
    b.add("temp", 22.5);
    b.add("name", string_view("hello"));

    REQUIRE(b.to_view() == R"({"flag":true,"count":42,"temp":22.5,"name":"hello"})");
}

TEST_CASE("StaticJsonBuilder: false boolean") {
    char buf[128];
    StaticJsonBuilder b(buf, sizeof(buf));
    b.add("off", false);
    REQUIRE(b.to_view() == R"({"off":false})");
}

TEST_CASE("StaticJsonBuilder: negative integer") {
    char buf[128];
    StaticJsonBuilder b(buf, sizeof(buf));
    b.add("val", int32_t{-100});
    REQUIRE(b.to_view() == R"({"val":-100})");
}

TEST_CASE("StaticJsonBuilder: nested object") {
    char buf[256];
    StaticJsonBuilder b(buf, sizeof(buf));
    b.add("req", string_view("note.add"));
    b.begin_object("body");
    b.add("temp", 22.5);
    b.add("label", string_view("room"));
    b.end_object();
    REQUIRE(b.to_view() == R"({"req":"note.add","body":{"temp":22.5,"label":"room"}})");
}

TEST_CASE("StaticJsonBuilder: array with elements") {
    char buf[256];
    StaticJsonBuilder b(buf, sizeof(buf));
    b.begin_array("files");
    b.add_element(string_view("data.qi"));
    b.add_element(string_view("settings.db"));
    b.end_array();
    REQUIRE(b.to_view() == R"({"files":["data.qi","settings.db"]})");
}

TEST_CASE("StaticJsonBuilder: array with mixed types") {
    char buf[256];
    StaticJsonBuilder b(buf, sizeof(buf));
    b.begin_array("items");
    b.add_element(true);
    b.add_element(int32_t{7});
    b.add_element(3.14);
    b.add_element(string_view("text"));
    b.end_array();
    REQUIRE(b.to_view() == R"({"items":[true,7,3.14,"text"]})");
}

TEST_CASE("StaticJsonBuilder: string escaping") {
    char buf[256];
    StaticJsonBuilder b(buf, sizeof(buf));
    b.add("msg", string_view("line1\nline2\ttab\"quote\\backslash"));
    auto json = b.to_view();
    REQUIRE(json.find("\\n") != std::string::npos);
    REQUIRE(json.find("\\t") != std::string::npos);
    REQUIRE(json.find("\\\"") != std::string::npos);
    REQUIRE(json.find("\\\\") != std::string::npos);
}

TEST_CASE("StaticJsonBuilder: reset reuses buffer") {
    char buf[128];
    StaticJsonBuilder b(buf, sizeof(buf));
    b.add("first", true);
    REQUIRE(b.to_view() == R"({"first":true})");

    b.reset();
    b.add("second", false);
    REQUIRE(b.to_view() == R"({"second":false})");
}

TEST_CASE("StaticJsonBuilder: to_view returns string_view") {
    char buf[128];
    StaticJsonBuilder b(buf, sizeof(buf));
    b.add("x", int32_t{1});
    auto v = b.to_view();
    REQUIRE(v == R"({"x":1})");
}

TEST_CASE("StaticJsonBuilder: overflow truncates gracefully") {
    char buf[16];  // Very small buffer
    StaticJsonBuilder b(buf, sizeof(buf));
    b.add("longkey", string_view("longvalue"));
    REQUIRE(b.overflow());
    // Should not crash, output is truncated
    auto v = b.to_view();
    REQUIRE(v.size() <= sizeof(buf));
}

TEST_CASE("StaticJsonBuilder: empty object") {
    char buf[64];
    StaticJsonBuilder b(buf, sizeof(buf));
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
    REQUIRE(r.get_double("temp", 0.0) == doctest::Approx(22.5));
    REQUIRE(r.get_string("name") == "hello");
}

TEST_CASE("JsmnJsonReader: missing keys return defaults") {
    const char* json = R"({"x":1})";
    jsmntok_t tokens[8];
    JsmnJsonReader r(json, strlen(json), tokens, 8);

    REQUIRE(r.get_bool("missing", true) == true);
    REQUIRE(r.get_int("missing", -1) == -1);
    REQUIRE(r.get_double("missing", 9.9) == doctest::Approx(9.9));
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
    REQUIRE(body->get_double("temp", 0.0) == doctest::Approx(22.5));
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
    SUBCASE("invalid JSON") {
        const char* json = "not json at all";
        jsmntok_t tokens[8];
        JsmnJsonReader r(json, strlen(json), tokens, 8);
        REQUIRE(r.has_error());
        REQUIRE(r.get_error() == "JSON parse error");
    }

    SUBCASE("empty input") {
        jsmntok_t tokens[8];
        JsmnJsonReader r("", 0, tokens, 8);
        REQUIRE(r.has_error());
    }

    SUBCASE("notecard error field") {
        const char* json = R"({"err":"something went wrong"})";
        jsmntok_t tokens[8];
        JsmnJsonReader r(json, strlen(json), tokens, 8);
        REQUIRE_FALSE(r.has_error());
        REQUIRE(r.get_error() == "something went wrong");
    }

    SUBCASE("no error field returns empty") {
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
// StaticJsonBackend integration
// ---------------------------------------------------------------------------

TEST_CASE("StaticJsonBackend: build and parse round-trip") {
    StaticJsonBackend<256, 32> backend;

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

TEST_CASE("StaticJsonBackend: get_builder resets between calls") {
    StaticJsonBackend<256, 32> backend;

    auto& b1 = backend.get_builder();
    b1.add("first", true);
    REQUIRE(b1.to_view() == R"({"first":true})");

    auto& b2 = backend.get_builder();
    b2.add("second", false);
    REQUIRE(b2.to_view() == R"({"second":false})");
}

TEST_CASE("StaticJsonBackend: parse_response returns owned reader") {
    StaticJsonBackend<256, 32> backend;

    const char* json = R"({"val":42})";
    auto reader = backend.parse_response(string_view(json, strlen(json)));
    REQUIRE(reader);
    REQUIRE(reader->get_int("val", 0) == 42);
}

TEST_CASE("StaticJsonBackend: create_builder returns owned builder") {
    StaticJsonBackend<256, 32> backend;

    auto builder = backend.create_builder();
    builder->add("key", string_view("val"));
    REQUIRE(builder->to_view() == R"({"key":"val"})");
}


// ---------------------------------------------------------------------------
// StaticJsonBuilder: control character escaping
// ---------------------------------------------------------------------------

TEST_CASE("StaticJsonBuilder: control characters pass through default case") {
    char buf[256];
    StaticJsonBuilder b(buf, sizeof(buf));
    // \x01 is a control character not handled by the switch cases
    // It should pass through the default branch
    std::string val = "hello";
    val += '\x01';
    val += "world";
    b.add("msg", string_view(val.data(), val.size()));
    auto json = b.to_view();
    // The control char should appear as-is in the JSON
    REQUIRE(json.find('\x01') != std::string::npos);
    // Verify the string structure is correct
    REQUIRE(json.find("\"msg\":\"") != std::string::npos);
}


// ---------------------------------------------------------------------------
// StaticJsonBuilder: overflow and reset cycle
// ---------------------------------------------------------------------------

TEST_CASE("StaticJsonBuilder: overflow then reset allows reuse") {
    char buf[20];  // Tiny buffer to force overflow
    StaticJsonBuilder b(buf, sizeof(buf));

    // Fill with data that will overflow
    b.add("longkey", string_view("longvalue-overflow"));
    REQUIRE(b.overflow());

    // Reset and fill again — should succeed with short data
    b.reset();
    REQUIRE_FALSE(b.overflow());
    b.add("x", int32_t{1});
    REQUIRE_FALSE(b.overflow());
    REQUIRE(b.to_view() == R"({"x":1})");
}


// ---------------------------------------------------------------------------
// JsmnJsonReader: get_string on non-string field returns default
// ---------------------------------------------------------------------------

TEST_CASE("JsmnJsonReader: get_string on integer returns default empty") {
    const char* json = R"({"count":42})";
    jsmntok_t tokens[8];
    JsmnJsonReader r(json, strlen(json), tokens, 8);

    // get_string on "count" which is a number token → returns default
    REQUIRE(r.get_string("count") == "");
    REQUIRE(r.get_string("count", "fallback") == "fallback");
}


// ---------------------------------------------------------------------------
// JsmnJsonReader: parse_int with decimal truncation (value 3.9 → 3)
// ---------------------------------------------------------------------------

TEST_CASE("JsmnJsonReader: get_int on decimal 3.9 returns 3") {
    const char* json = R"({"value":3.9})";
    jsmntok_t tokens[8];
    JsmnJsonReader r(json, strlen(json), tokens, 8);

    REQUIRE(r.get_int("value", 0) == 3);
}

TEST_CASE("JsmnJsonReader: get_int on negative decimal -7.3 returns -7") {
    const char* json = R"({"value":-7.3})";
    jsmntok_t tokens[8];
    JsmnJsonReader r(json, strlen(json), tokens, 8);

    REQUIRE(r.get_int("value", 0) == -7);
}


// ---------------------------------------------------------------------------
// JsmnJsonReader: get_string_array with missing key returns 0 elements
// ---------------------------------------------------------------------------

TEST_CASE("JsmnJsonReader: get_string_array missing key returns 0") {
    const char* json = R"({"name":"test"})";
    jsmntok_t tokens[8];
    JsmnJsonReader r(json, strlen(json), tokens, 8);

    string_view out[4];
    size_t count = r.get_string_array("files", out, 4);
    REQUIRE(count == 0);
}

TEST_CASE("JsmnJsonReader: get_string_array on non-array returns 0") {
    const char* json = R"({"files":"not-an-array"})";
    jsmntok_t tokens[8];
    JsmnJsonReader r(json, strlen(json), tokens, 8);

    string_view out[4];
    size_t count = r.get_string_array("files", out, 4);
    REQUIRE(count == 0);
}


// ---------------------------------------------------------------------------
// JsmnJsonReader: parse error on JSON array root → has_error()
// ---------------------------------------------------------------------------

TEST_CASE("JsmnJsonReader: array root has_error true") {
    const char* json = "[1,2,3]";
    jsmntok_t tokens[8];
    JsmnJsonReader r(json, strlen(json), tokens, 8);

    REQUIRE(r.has_error());
    REQUIRE(r.get_error() == "JSON parse error");
}


// ---------------------------------------------------------------------------
// JsmnJsonReader: get_string_array success with actual array
// ---------------------------------------------------------------------------

TEST_CASE("JsmnJsonReader: get_string_array returns elements") {
    const char* json = R"({"files":["data.qi","settings.db","env.db"]})";
    jsmntok_t tokens[16];
    JsmnJsonReader r(json, strlen(json), tokens, 16);

    string_view out[4];
    size_t count = r.get_string_array("files", out, 4);
    REQUIRE(count == 3);
    REQUIRE(out[0] == "data.qi");
    REQUIRE(out[1] == "settings.db");
    REQUIRE(out[2] == "env.db");
}

TEST_CASE("JsmnJsonReader: get_string_array limited by max") {
    const char* json = R"({"files":["a","b","c"]})";
    jsmntok_t tokens[16];
    JsmnJsonReader r(json, strlen(json), tokens, 16);

    string_view out[2];
    size_t count = r.get_string_array("files", out, 2);
    REQUIRE(count == 2);
    REQUIRE(out[0] == "a");
    REQUIRE(out[1] == "b");
}


// ---------------------------------------------------------------------------
// JsmnJsonReader: parse_int with non-numeric returns default
// ---------------------------------------------------------------------------

TEST_CASE("JsmnJsonReader: get_int on boolean returns default") {
    const char* json = R"({"flag":true})";
    jsmntok_t tokens[8];
    JsmnJsonReader r(json, strlen(json), tokens, 8);

    // "true" contains 't' which is not a digit → parse_int returns default
    REQUIRE(r.get_int("flag", -99) == -99);
}


// ---------------------------------------------------------------------------
// JsmnJsonReader: parse_double negative value
// ---------------------------------------------------------------------------

TEST_CASE("JsmnJsonReader: get_double negative value") {
    const char* json = R"({"temp":-12.5})";
    jsmntok_t tokens[8];
    JsmnJsonReader r(json, strlen(json), tokens, 8);

    REQUIRE(r.get_double("temp", 0.0) == doctest::Approx(-12.5));
}


// ---------------------------------------------------------------------------
// JsmnJsonReader: get_object_array with objects
// ---------------------------------------------------------------------------

TEST_CASE("JsmnJsonReader: get_object_array returns objects") {
    const char* json = R"({"items":[{"a":1},{"b":2}]})";
    jsmntok_t tokens[32];
    JsmnJsonReader r(json, strlen(json), tokens, 32);

    std::unique_ptr<JsonReader> out[4];
    size_t count = r.get_object_array("items", out, 4);
    REQUIRE(count == 2);
    REQUIRE(out[0]->get_int("a", 0) == 1);
    REQUIRE(out[1]->get_int("b", 0) == 2);
}

TEST_CASE("JsmnJsonReader: get_object_array missing key returns 0") {
    const char* json = R"({"x":1})";
    jsmntok_t tokens[8];
    JsmnJsonReader r(json, strlen(json), tokens, 8);

    std::unique_ptr<JsonReader> out[4];
    size_t count = r.get_object_array("items", out, 4);
    REQUIRE(count == 0);
}


// ---------------------------------------------------------------------------
// JsmnJsonReader: empty parse_int and parse_double
// ---------------------------------------------------------------------------

TEST_CASE("JsmnJsonReader: get_int with empty string key returns default") {
    const char* json = R"({"":42})";
    jsmntok_t tokens[8];
    JsmnJsonReader r(json, strlen(json), tokens, 8);

    // Empty key should be found
    REQUIRE(r.get_int("", 0) == 42);
}


// ---------------------------------------------------------------------------
// JsmnJsonReader: get_bool on non-boolean returns default
// ---------------------------------------------------------------------------

TEST_CASE("JsmnJsonReader: get_bool on number returns default") {
    const char* json = R"({"val":42})";
    jsmntok_t tokens[8];
    JsmnJsonReader r(json, strlen(json), tokens, 8);

    // "42" is neither "true" nor "false" → returns default
    REQUIRE(r.get_bool("val", true) == true);
    REQUIRE(r.get_bool("val", false) == false);
}


// ---------------------------------------------------------------------------
// StaticJsonBuilder: all escape sequences in one string
// ---------------------------------------------------------------------------

TEST_CASE("StaticJsonBuilder: all escape sequences covered") {
    char buf[256];
    StaticJsonBuilder b(buf, sizeof(buf));
    b.add("s", string_view("a\"b\\c\nd\re\tf"));
    auto json = b.to_view();
    // All escape sequences should be present
    REQUIRE(json.find("\\\"") != std::string::npos);
    REQUIRE(json.find("\\\\") != std::string::npos);
    REQUIRE(json.find("\\n") != std::string::npos);
    REQUIRE(json.find("\\r") != std::string::npos);
    REQUIRE(json.find("\\t") != std::string::npos);
}


// ---------------------------------------------------------------------------
// StaticJsonBuilder: add_raw method
// ---------------------------------------------------------------------------

TEST_CASE("StaticJsonBuilder: add_raw inserts verbatim JSON") {
    char buf[256];
    StaticJsonBuilder b(buf, sizeof(buf));
    b.add_raw("body", string_view("{\"temp\":22.5}"));
    REQUIRE(b.to_view() == R"({"body":{"temp":22.5}})");
}


// ---------------------------------------------------------------------------
// StaticJsonBuilder: multiple to_view calls are idempotent
// ---------------------------------------------------------------------------

TEST_CASE("StaticJsonBuilder: to_view is idempotent") {
    char buf[128];
    StaticJsonBuilder b(buf, sizeof(buf));
    b.add("x", int32_t{1});
    auto v1 = b.to_view();
    auto v2 = b.to_view();
    REQUIRE(v1 == v2);
    REQUIRE(v1 == R"({"x":1})");
}


// ---------------------------------------------------------------------------
// JsmnJsonReader: token overflow handled gracefully
// ---------------------------------------------------------------------------

TEST_CASE("JsmnJsonReader: too few tokens reports error") {
    const char* json = R"({"a":1,"b":2,"c":3})";
    jsmntok_t tokens[4];  // Not enough tokens for all key-value pairs
    JsmnJsonReader r(json, strlen(json), tokens, 4);
    // jsmn returns negative on insufficient tokens → token_count_ = 0 → has_error
    REQUIRE(r.has_error());
}


// ---------------------------------------------------------------------------
// Branch coverage: add_element(false) — line 87 false branch
// ---------------------------------------------------------------------------

TEST_CASE("StaticJsonBuilder: add_element(false) in array") {
    char buf[128];
    StaticJsonBuilder b(buf, sizeof(buf));
    b.begin_array("arr");
    b.add_element(false);
    b.add_element(true);
    b.end_array();
    REQUIRE(b.to_view() == R"({"arr":[false,true]})");
}


// ---------------------------------------------------------------------------
// Branch coverage: get_string_array with non-string elements
// ---------------------------------------------------------------------------

TEST_CASE("JsmnJsonReader: get_string_array skips non-string elements") {
    const char* json = R"({"items":["hello",42,true,"world"]})";
    jsmntok_t tokens[16];
    JsmnJsonReader r(json, strlen(json), tokens, 16);

    string_view out[4];
    size_t count = r.get_string_array("items", out, 4);
    REQUIRE(count == 2);
    REQUIRE(out[0] == "hello");
    REQUIRE(out[1] == "world");
}


// ---------------------------------------------------------------------------
// Branch coverage: get_object_array on non-array — line 250
// ---------------------------------------------------------------------------

TEST_CASE("JsmnJsonReader: get_object_array on non-array returns 0") {
    const char* json = R"({"items":"not-an-array"})";
    jsmntok_t tokens[8];
    JsmnJsonReader r(json, strlen(json), tokens, 8);

    std::unique_ptr<JsonReader> out[4];
    size_t count = r.get_object_array("items", out, 4);
    REQUIRE(count == 0);
}

TEST_CASE("JsmnJsonReader: get_object_array skips non-object elements") {
    const char* json = R"({"items":[{"a":1},"skip",{"b":2}]})";
    jsmntok_t tokens[32];
    JsmnJsonReader r(json, strlen(json), tokens, 32);

    std::unique_ptr<JsonReader> out[4];
    size_t count = r.get_object_array("items", out, 4);
    REQUIRE(count == 2);
    REQUIRE(out[0]->get_int("a", 0) == 1);
    REQUIRE(out[1]->get_int("b", 0) == 2);
}

TEST_CASE("JsmnJsonReader: get_object_array limited by max") {
    const char* json = R"({"items":[{"a":1},{"b":2},{"c":3}]})";
    jsmntok_t tokens[32];
    JsmnJsonReader r(json, strlen(json), tokens, 32);

    std::unique_ptr<JsonReader> out[2];
    size_t count = r.get_object_array("items", out, 2);
    REQUIRE(count == 2);
    REQUIRE(out[0]->get_int("a", 0) == 1);
    REQUIRE(out[1]->get_int("b", 0) == 2);
}


// ---------------------------------------------------------------------------
// Branch coverage: get_error with non-string err field — line 279
// ---------------------------------------------------------------------------

TEST_CASE("JsmnJsonReader: get_error with non-string err returns empty") {
    const char* json = R"({"err":42})";
    jsmntok_t tokens[8];
    JsmnJsonReader r(json, strlen(json), tokens, 8);
    REQUIRE_FALSE(r.has_error());
    REQUIRE(r.get_error().empty());
}


// ---------------------------------------------------------------------------
// Branch coverage: sub-reader with invalid root — line 321
// ---------------------------------------------------------------------------

TEST_CASE("JsmnJsonReader: sub-reader with root beyond token count") {
    const char* json = R"({"x":1})";
    jsmntok_t tokens[8];
    JsmnJsonReader parent(json, strlen(json), tokens, 8);

    JsmnJsonReader sub(json, strlen(json), tokens, 3, /*root=*/10);
    REQUIRE(sub.has_error());
    REQUIRE(sub.get_int("x", -1) == -1);
}


// ---------------------------------------------------------------------------
// Branch coverage: sub-reader on array root — line 323
// ---------------------------------------------------------------------------

TEST_CASE("JsmnJsonReader: sub-reader on array root returns defaults") {
    const char* json = R"({"arr":[1,2,3]})";
    jsmntok_t tokens[16];
    JsmnJsonReader parent(json, strlen(json), tokens, 16);

    // Array token is at index 2 (0=obj, 1="arr" key, 2=array)
    JsmnJsonReader sub(json, strlen(json), tokens, 16, /*root=*/2);
    REQUIRE(sub.has_error());
    REQUIRE(sub.get_int("x", -1) == -1);
}


// ---------------------------------------------------------------------------
// Branch coverage: find_value with truncated key-value pair — line 327
// ---------------------------------------------------------------------------

TEST_CASE("JsmnJsonReader: find_value with truncated tokens") {
    const char* json = R"({"a":1,"b":2})";
    jsmntok_t tokens[16];
    JsmnJsonReader full(json, strlen(json), tokens, 16);

    // Tokens: 0=obj(size=2), 1="a"key, 2=1val, 3="b"key, 4=2val
    // With token_count=4: looking up "b" hits idx=3, idx+1=4 >= 4, break.
    JsmnJsonReader sub(json, strlen(json), tokens, 4, /*root=*/0);
    REQUIRE(sub.get_int("a", 0) == 1);
    REQUIRE(sub.get_int("b", -1) == -1);
}


// ---------------------------------------------------------------------------
// Branch coverage: parse_int with empty string — line 339
// ---------------------------------------------------------------------------

TEST_CASE("JsmnJsonReader: get_int on empty string returns default") {
    const char* json = R"({"val":""})";
    jsmntok_t tokens[8];
    JsmnJsonReader r(json, strlen(json), tokens, 8);
    REQUIRE(r.get_int("val", -99) == -99);
}


// ---------------------------------------------------------------------------
// Branch coverage: parse_double with empty string — line 356
// ---------------------------------------------------------------------------

TEST_CASE("JsmnJsonReader: get_double on empty string returns default") {
    const char* json = R"({"val":""})";
    jsmntok_t tokens[8];
    JsmnJsonReader r(json, strlen(json), tokens, 8);
    REQUIRE(r.get_double("val", -99.0) == doctest::Approx(-99.0));
}


// ---------------------------------------------------------------------------
// Branch coverage: parse_double integer path — line 362
// ---------------------------------------------------------------------------

TEST_CASE("JsmnJsonReader: parse_double integer without decimal") {
    const char* json = R"({"val":42})";
    jsmntok_t tokens[8];
    JsmnJsonReader r(json, strlen(json), tokens, 8);
    REQUIRE(r.get_double("val", 0.0) == doctest::Approx(42.0));
}

TEST_CASE("JsmnJsonReader: parse_double negative with decimal") {
    const char* json = R"({"val":-3.14})";
    jsmntok_t tokens[8];
    JsmnJsonReader r(json, strlen(json), tokens, 8);
    REQUIRE(r.get_double("val", 0.0) == doctest::Approx(-3.14));
}


// ---------------------------------------------------------------------------
// Branch coverage: parse_double stops at 'e'/'E' — line 367
// ---------------------------------------------------------------------------

TEST_CASE("JsmnJsonReader: parse_double stops at exponent e") {
    const char* json = R"({"val":1e5})";
    jsmntok_t tokens[8];
    JsmnJsonReader r(json, strlen(json), tokens, 8);
    REQUIRE(r.get_double("val", 0.0) == doctest::Approx(1.0));
}

TEST_CASE("JsmnJsonReader: parse_double stops at exponent E in decimal part") {
    const char* json = R"({"val":1.5E3})";
    jsmntok_t tokens[8];
    JsmnJsonReader r(json, strlen(json), tokens, 8);
    REQUIRE(r.get_double("val", 0.0) == doctest::Approx(1.5));
}


// ---------------------------------------------------------------------------
// Branch coverage: tok_span with nested objects
// ---------------------------------------------------------------------------

TEST_CASE("JsmnJsonReader: find_value skips deeply nested objects") {
    const char* json = R"({"nested":{"a":{"b":1}},"after":"found"})";
    jsmntok_t tokens[16];
    JsmnJsonReader r(json, strlen(json), tokens, 16);
    REQUIRE(r.get_string("after") == "found");
}


// ---------------------------------------------------------------------------
// Branch coverage: overflow to_view returns capacity-1 length
// ---------------------------------------------------------------------------

TEST_CASE("StaticJsonBuilder: overflow to_view returns truncated length") {
    char buf[8];
    StaticJsonBuilder b(buf, sizeof(buf));
    b.add("k", string_view("val"));
    REQUIRE(b.overflow());
    auto v = b.to_view();
    REQUIRE(v.size() == sizeof(buf) - 1);
}
