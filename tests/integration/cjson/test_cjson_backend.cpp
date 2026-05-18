// Integration test for the cJSON backend.
// Verifies request building, response parsing, nested objects, and error handling.
//
// This file compiles into two binaries:
//   - host: `note-cpp-integration-backends` (tests/CMakeLists.txt)
//   - device: tests/integration/firmware (via test_src_filter)
// doctest's main comes from tests/doctest_main.cpp (host) or
// tests/integration/firmware/test/main.cpp (device).

#include <doctest.h>

#include <note/backends/cjson.hpp>

#include <cmath>
#include <string>

using namespace note::backends;

TEST_CASE("cjson/builder/simple") {
    CjsonBackend backend;
    auto builder = backend.create_builder();
    builder->add("req", "hub.set");
    builder->add("product", "com.example.app");
    builder->add("mode", "periodic");
    builder->add("outbound", int32_t{60});
    auto json = builder->to_view();

    CHECK(json.find("\"req\":\"hub.set\"") != std::string::npos);
    CHECK(json.find("\"product\":\"com.example.app\"") != std::string::npos);
    CHECK(json.find("\"outbound\":60") != std::string::npos);
}

TEST_CASE("cjson/builder/types") {
    CjsonBackend backend;
    auto builder = backend.create_builder();
    builder->add("flag", true);
    builder->add("count", int32_t{42});
    builder->add("value", 3.14);
    builder->add("name", "test");
    auto json = builder->to_view();

    CHECK(json.find("\"flag\":true") != std::string::npos);
    CHECK(json.find("\"count\":42") != std::string::npos);
    CHECK(json.find("\"value\":3.14") != std::string::npos);
    CHECK(json.find("\"name\":\"test\"") != std::string::npos);
}

TEST_CASE("cjson/builder/nested_object") {
    CjsonBackend backend;
    auto builder = backend.create_builder();
    builder->add("req", "note.add");
    builder->begin_object("body");
    builder->add("temp", 22.5);
    builder->add("humidity", int32_t{60});
    builder->end_object();
    auto json = builder->to_view();

    CHECK(json.find("\"body\":{") != std::string::npos);
    CHECK(json.find("\"temp\":22.5") != std::string::npos);
    CHECK(json.find("\"humidity\":60") != std::string::npos);
}

TEST_CASE("cjson/reader/simple") {
    CjsonBackend backend;
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

TEST_CASE("cjson/reader/numbers") {
    CjsonBackend backend;
    auto reader = backend.parse_response(R"({"int_val":42,"float_val":3.14,"neg":-7})");

    CHECK(reader->get_int("int_val") == 42);
    CHECK(std::abs(reader->get_double("float_val") - 3.14) < 0.001);
    CHECK(reader->get_int("neg") == -7);
}

TEST_CASE("cjson/reader/nested_object") {
    CjsonBackend backend;
    auto reader = backend.parse_response(
        R"({"body":{"temp":22.5,"label":"room-1"},"file":"data.qi"})");

    CHECK(reader->get_string("file") == "data.qi");

    auto body = reader->get_object("body");
    REQUIRE(body != nullptr);
    CHECK(std::abs(body->get_double("temp") - 22.5) < 0.001);
    CHECK(body->get_string("label") == "room-1");

    CHECK(reader->get_object("missing") == nullptr);
}

TEST_CASE("cjson/reader/error") {
    CjsonBackend backend;

    auto reader = backend.parse_response(R"({"err":"file not found"})");
    CHECK(reader->get_error() == "file not found");

    auto bad = backend.parse_response("not json");
    CHECK(bad->has_error());
}

TEST_CASE("cjson/reader/defaults") {
    CjsonBackend backend;
    auto reader = backend.parse_response("{}");

    CHECK(reader->get_bool("x", true) == true);
    CHECK(reader->get_int("x", 99) == 99);
    CHECK(std::abs(reader->get_double("x", 1.5) - 1.5) < 0.001);
    CHECK(reader->get_string("x", "default") == "default");
}

TEST_CASE("cjson/round_trip") {
    CjsonBackend backend;

    auto builder = backend.create_builder();
    builder->add("req", "note.add");
    builder->add("file", "sensors.qo");
    builder->begin_object("body");
    builder->add("temp", 22.5);
    builder->add("humidity", int32_t{60});
    builder->end_object();
    auto json = builder->to_view();

    auto reader = backend.parse_response(json);
    CHECK(reader->get_string("req") == "note.add");
    CHECK(reader->get_string("file") == "sensors.qo");

    auto body = reader->get_object("body");
    REQUIRE(body != nullptr);
    CHECK(std::abs(body->get_double("temp") - 22.5) < 0.001);
    CHECK(body->get_int("humidity") == 60);
}

// Wrong-type fallbacks: when a key exists but its value is the wrong type
// for the requested getter, the reader must return the supplied default.
// The default-on-missing-key path is already covered by `cjson/reader/defaults`;
// these subcases exercise the parallel branches where `cJSON_Is<T>` returns
// false.
TEST_CASE("cjson/reader/wrong_type_returns_default") {
    CjsonBackend backend;
    // s is a string, n is a number, b is a bool, a is an array, o is an object.
    auto reader = backend.parse_response(
        R"({"s":"hi","n":42,"b":true,"a":[1,2],"o":{"k":1}})");

    SUBCASE("get_bool on non-bool returns default") {
        CHECK(reader->get_bool("s", true)  == true);
        CHECK(reader->get_bool("s", false) == false);
        CHECK(reader->get_bool("n", true)  == true);
    }
    SUBCASE("get_int on non-number returns default") {
        CHECK(reader->get_int("s", 99) == 99);
        CHECK(reader->get_int("b", 99) == 99);
    }
    SUBCASE("get_double on non-number returns default") {
        CHECK(std::abs(reader->get_double("s", 1.25) - 1.25) < 0.001);
        CHECK(std::abs(reader->get_double("b", 1.25) - 1.25) < 0.001);
    }
    SUBCASE("get_string on non-string returns default") {
        CHECK(reader->get_string("n", "fallback") == "fallback");
        CHECK(reader->get_string("b", "fallback") == "fallback");
    }
    SUBCASE("get_string_array on non-array returns 0") {
        note::string_view out[4];
        CHECK(reader->get_string_array("s", out, 4) == 0);
        CHECK(reader->get_string_array("n", out, 4) == 0);
    }
    SUBCASE("get_object_array on non-array returns 0") {
        std::unique_ptr<note::JsonReader> out[4];
        CHECK(reader->get_object_array("s", out, 4) == 0);
    }
    SUBCASE("get_object on non-object returns nullptr") {
        CHECK(reader->get_object("s") == nullptr);
        CHECK(reader->get_object("a") == nullptr);
    }
}

TEST_CASE("cjson/reader/error_message_paths") {
    CjsonBackend backend;

    SUBCASE("parse failure: has_error true, err_message default") {
        auto reader = backend.parse_response("not json at all");
        REQUIRE(reader->has_error());
        // Null root -> "JSON parse error" sentinel.
        CHECK(reader->get_error() == "JSON parse error");
    }
    SUBCASE("err field with non-string value: returns empty view") {
        // err present but the wrong type — get_error must NOT mis-interpret.
        auto reader = backend.parse_response(R"({"err":42})");
        CHECK_FALSE(reader->has_error());
        CHECK(reader->get_error().empty());
    }
    SUBCASE("no err field at all: empty view") {
        auto reader = backend.parse_response(R"({"ok":true})");
        CHECK(reader->get_error().empty());
    }
}

TEST_CASE("cjson/builder/reset_after_use_rebuilds_tree") {
    // reset() on a builder that already has a tree must dispose of the old
    // root and start fresh — covers the `if (root_) cJSON_Delete(root_)`
    // branch when root_ is non-null.
    CjsonBackend backend;
    auto builder = backend.create_builder();
    builder->add("first", "a");
    builder->reset();
    builder->add("second", "b");
    auto json = builder->to_view();
    auto reader = backend.parse_response(json);
    CHECK(reader->get_string("first", "MISSING") == "MISSING");
    CHECK(reader->get_string("second") == "b");
}
