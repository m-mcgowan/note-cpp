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

TEST_CASE("cjson/reader/missing_key_paths") {
    CjsonBackend backend;
    auto reader = backend.parse_response(R"({"a":[1,2,3],"o":{"k":1}})");
    REQUIRE_FALSE(reader->has_error());

    SUBCASE("get_string_array on missing key returns 0") {
        note::string_view out[4];
        CHECK(reader->get_string_array("nope", out, 4) == 0);
    }
    SUBCASE("get_object_array on missing key returns 0") {
        std::unique_ptr<note::JsonReader> out[4];
        CHECK(reader->get_object_array("nope", out, 4) == 0);
    }
    SUBCASE("get_object on missing key returns nullptr") {
        CHECK(reader->get_object("nope") == nullptr);
    }
    SUBCASE("has on missing key returns false") {
        CHECK_FALSE(reader->has("nope"));
    }
    SUBCASE("has on present key returns true") {
        CHECK(reader->has("a"));
        CHECK(reader->has("o"));
    }
}

// Mixed-typed array exercises the per-element type check inside
// get_string_array / get_object_array (skipping non-matching elements).
TEST_CASE("cjson/reader/mixed_array_skips_wrong_type_elements") {
    CjsonBackend backend;
    auto reader = backend.parse_response(
        R"({"sa":["a",1,"b",true,"c"],"oa":[{"k":1},2,{"k":3},"x"]})");
    REQUIRE_FALSE(reader->has_error());

    SUBCASE("get_string_array picks only string elements") {
        note::string_view out[8];
        size_t n = reader->get_string_array("sa", out, 8);
        REQUIRE(n == 3);
        CHECK(out[0] == "a");
        CHECK(out[1] == "b");
        CHECK(out[2] == "c");
    }
    SUBCASE("get_object_array picks only object elements") {
        std::unique_ptr<note::JsonReader> out[8];
        size_t n = reader->get_object_array("oa", out, 8);
        REQUIRE(n == 2);
        CHECK(out[0]->get_int("k", -1) == 1);
        CHECK(out[1]->get_int("k", -1) == 3);
    }
    SUBCASE("get_string_array honours max cap") {
        note::string_view out[2];
        size_t n = reader->get_string_array("sa", out, 2);
        CHECK(n == 2);  // hits the n < max loop guard
        CHECK(out[0] == "a");
        CHECK(out[1] == "b");
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

// Verifies CjsonBackend::start_response / finish_response — the SAX
// events-in surface that drives the buffered/tree response path on
// JSONB or on any non-text-buffered transport. Each subcase builds the
// SAME response shape twice (text via parse_response, SAX events via
// start/finish_response) and asserts both readers yield identical
// fields. This is the cJSON half of the Tree×JSONB parity test.
TEST_CASE("cjson/start_response/parity_with_parse_response") {
    CjsonBackend backend;
    char buf[512];  // unused for cjson — sink builds the tree directly.

    SUBCASE("flat object — scalars only") {
        const char* text = R"({"req":"card.status","id":42,"ok":true,"aux":null,"temp":22.5})";

        auto& sax_sink = backend.start_response(note::span<char>(buf, sizeof(buf)));
        sax_sink.on_object_begin("");
        sax_sink.on_string("req",  "card.status");
        sax_sink.on_int   ("id",   42);
        sax_sink.on_bool  ("ok",   true);
        sax_sink.on_null  ("aux");
        sax_sink.on_float ("temp", 22.5);
        sax_sink.on_object_end("");
        auto& sax_reader = backend.finish_response();

        auto text_reader = backend.parse_response(text);

        CHECK(sax_reader.get_string("req")  == text_reader->get_string("req"));
        CHECK(sax_reader.get_int("id")      == text_reader->get_int("id"));
        CHECK(sax_reader.get_bool("ok")     == text_reader->get_bool("ok"));
        CHECK(sax_reader.has("aux")         == text_reader->has("aux"));
        CHECK(sax_reader.get_double("temp") == text_reader->get_double("temp"));
        CHECK_FALSE(sax_reader.has_error());
    }

    SUBCASE("nested object") {
        const char* text = R"({"req":"note.add","body":{"temp":22.5,"hum":60}})";

        auto& sink = backend.start_response(note::span<char>(buf, sizeof(buf)));
        sink.on_object_begin("");
        sink.on_string("req", "note.add");
        sink.on_object_begin("body");
        sink.on_float("temp", 22.5);
        sink.on_int  ("hum",  60);
        sink.on_object_end("body");
        sink.on_object_end("");
        auto& sax_reader = backend.finish_response();

        auto text_reader = backend.parse_response(text);

        CHECK(sax_reader.get_string("req") == text_reader->get_string("req"));
        auto sax_body  = sax_reader.get_object("body");
        auto text_body = text_reader->get_object("body");
        REQUIRE(sax_body);
        REQUIRE(text_body);
        CHECK(sax_body->get_double("temp") == text_body->get_double("temp"));
        CHECK(sax_body->get_int   ("hum")  == text_body->get_int   ("hum"));
    }

    SUBCASE("string array") {
        const char* text = R"({"files":["a","b","c"]})";

        auto& sink = backend.start_response(note::span<char>(buf, sizeof(buf)));
        sink.on_object_begin("");
        sink.on_array_begin("files");
        sink.on_string("files", "a");
        sink.on_string("files", "b");
        sink.on_string("files", "c");
        sink.on_array_end("files");
        sink.on_object_end("");
        auto& sax_reader = backend.finish_response();

        auto text_reader = backend.parse_response(text);

        note::string_view sax_arr[8], text_arr[8];
        auto sax_n  = sax_reader.get_string_array("files",  sax_arr,  8);
        auto text_n = text_reader->get_string_array("files", text_arr, 8);
        CHECK(sax_n == text_n);
        REQUIRE(sax_n == 3);
        for (size_t i = 0; i < sax_n; ++i) {
            CHECK(sax_arr[i] == text_arr[i]);
        }
    }

    SUBCASE("err field is surfaced through both paths") {
        auto& sink = backend.start_response(note::span<char>(buf, sizeof(buf)));
        sink.on_object_begin("");
        sink.on_string("err", "{io}");
        sink.on_object_end("");
        auto& sax_reader = backend.finish_response();
        CHECK(sax_reader.get_error() == "{io}");
    }

    SUBCASE("rearm between transactions disposes prior tree") {
        // First response.
        auto& sink1 = backend.start_response(note::span<char>(buf, sizeof(buf)));
        sink1.on_object_begin("");
        sink1.on_int("x", 1);
        sink1.on_object_end("");
        auto& r1 = backend.finish_response();
        CHECK(r1.get_int("x") == 1);

        // Second response — must not leak the first.
        auto& sink2 = backend.start_response(note::span<char>(buf, sizeof(buf)));
        sink2.on_object_begin("");
        sink2.on_int("y", 2);
        sink2.on_object_end("");
        auto& r2 = backend.finish_response();
        CHECK(r2.get_int("y") == 2);
        CHECK(r2.has("x") == false);
    }
}
