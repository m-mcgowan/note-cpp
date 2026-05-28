// Tests for note::detail::SaxToTextSink — the JsonSink that re-serializes
// SAX events back into JSON text. The sink is the default impl behind
// JsonBackend::start_response / finish_response: it bridges the
// SAX-events-in surface to text-shaped backends (StaticJsonBackend +
// anything that doesn't override the defaults).
//
// We exercise representative event streams that mirror what the JSON
// lexer + JSONB parser actually emit, then re-parse the produced text
// with the same SAX parser to confirm round-trip equivalence.

#include <doctest.h>

#include <note/json.hpp>
#include <note/json_sax.hpp>

#include <string>
#include <vector>

namespace {

using note::detail::SaxToTextSink;

// Drive a sink through a small canned event sequence corresponding to
// a single response object. Returns the serialized JSON text.
std::string serialize_events(SaxToTextSink& sink) {
    sink.on_object_begin("");        // root open
    sink.on_string("req",  "card.status");
    sink.on_int   ("id",   42);
    sink.on_bool  ("ok",   true);
    sink.on_float ("temp", 22.5);
    sink.on_null  ("aux");
    sink.on_object_end("");
    return std::string(sink.view());
}

// Recorder for round-trip parity. `numbers` counts all numeric
// deliveries — the buffer-based sax_parse() emits on_number with raw
// lexemes, while the lexer-based sax_lex* emits typed on_int/on_float.
// We count both so the test is parser-agnostic.
struct CountSink : note::JsonSink {
    int strings = 0, numbers = 0, bools = 0, nulls = 0;
    int obj_begin = 0, obj_end = 0, arr_begin = 0, arr_end = 0;
    std::vector<std::string> keys;

    void on_string(note::string_view k, note::string_view) override { ++strings; keys.emplace_back(k); }
    void on_int   (note::string_view k, note::json_int_t)  override { ++numbers; keys.emplace_back(k); }
    void on_float (note::string_view k, double)            override { ++numbers; keys.emplace_back(k); }
    void on_number(note::string_view k, note::string_view) override { ++numbers; keys.emplace_back(k); }
    void on_bool  (note::string_view k, bool)              override { ++bools; keys.emplace_back(k); }
    void on_null  (note::string_view k)                    override { ++nulls; keys.emplace_back(k); }
    void on_object_begin(note::string_view) override { ++obj_begin; }
    void on_object_end  (note::string_view) override { ++obj_end; }
    void on_array_begin (note::string_view) override { ++arr_begin; }
    void on_array_end   (note::string_view) override { ++arr_end; }
};

} // namespace

TEST_CASE("SaxToTextSink serializes a flat object") {
    char buf[256];
    SaxToTextSink sink;
    sink.rearm(note::span<char>(buf, sizeof(buf)));

    auto text = serialize_events(sink);
    CHECK_FALSE(sink.overflow());

    // Sanity-check the produced text shape. Property-style — don't
    // pin exact float formatting (could be "22.5" or "22.500000").
    CHECK(text.front() == '{');
    CHECK(text.back()  == '}');
    CHECK(text.find("\"req\":\"card.status\"") != std::string::npos);
    CHECK(text.find("\"id\":42")              != std::string::npos);
    CHECK(text.find("\"ok\":true")            != std::string::npos);
    CHECK(text.find("\"aux\":null")           != std::string::npos);
    CHECK(text.find("\"temp\":")              != std::string::npos);
}

TEST_CASE("SaxToTextSink round-trips through the SAX parser") {
    char buf[256];
    SaxToTextSink sink;
    sink.rearm(note::span<char>(buf, sizeof(buf)));

    auto text = serialize_events(sink);

    CountSink rec;
    auto err = note::sax_parse(text, rec);
    CHECK(err.empty());

    CHECK(rec.obj_begin == 1);
    CHECK(rec.obj_end   == 1);
    CHECK(rec.strings   == 1);
    CHECK(rec.numbers   == 2);  // id (int) + temp (float)
    CHECK(rec.bools     == 1);
    CHECK(rec.nulls     == 1);
}

TEST_CASE("SaxToTextSink escapes JSON string values") {
    char buf[128];
    SaxToTextSink sink;
    sink.rearm(note::span<char>(buf, sizeof(buf)));

    sink.on_object_begin("");
    sink.on_string("body", "line1\nline2\t\"quoted\"");
    sink.on_object_end("");
    auto text = std::string(sink.view());

    // Escapes must land in the text representation.
    CHECK(text.find("\\n") != std::string::npos);
    CHECK(text.find("\\t") != std::string::npos);
    CHECK(text.find("\\\"") != std::string::npos);

    // Re-parsing must succeed and recover the original value.
    struct Capture : note::JsonSink {
        std::string captured;
        void on_string(note::string_view, note::string_view v) override {
            captured.assign(v.data(), v.size());
        }
    } cap;
    auto err = note::sax_parse(text, cap);
    CHECK(err.empty());
    CHECK(cap.captured == "line1\nline2\t\"quoted\"");
}

TEST_CASE("SaxToTextSink handles nested objects") {
    char buf[256];
    SaxToTextSink sink;
    sink.rearm(note::span<char>(buf, sizeof(buf)));

    // {"req":"note.add","body":{"temp":22.5,"hum":60}}
    sink.on_object_begin("");
    sink.on_string("req", "note.add");
    sink.on_object_begin("body");
    sink.on_float("temp", 22.5);
    sink.on_int("hum", 60);
    sink.on_object_end("body");
    sink.on_object_end("");

    auto text = std::string(sink.view());
    CHECK_FALSE(sink.overflow());
    CHECK(text.find("\"body\":{") != std::string::npos);
    CHECK(text.find("\"hum\":60") != std::string::npos);

    CountSink rec;
    auto err = note::sax_parse(text, rec);
    CHECK(err.empty());
    CHECK(rec.obj_begin == 2);  // root + body
    CHECK(rec.obj_end   == 2);
}

TEST_CASE("SaxToTextSink handles arrays of values") {
    char buf[256];
    SaxToTextSink sink;
    sink.rearm(note::span<char>(buf, sizeof(buf)));

    // {"files":["a","b","c"]}
    sink.on_object_begin("");
    sink.on_array_begin("files");
    // Inside an array, SAX adapters repeat the array's key on each
    // element — SaxToTextSink relies on its own depth stack to know
    // it's in array context and suppresses key prefixes.
    sink.on_string("files", "a");
    sink.on_string("files", "b");
    sink.on_string("files", "c");
    sink.on_array_end("files");
    sink.on_object_end("");

    auto text = std::string(sink.view());
    CHECK_FALSE(sink.overflow());
    CHECK(text.find("\"files\":[\"a\",\"b\",\"c\"]") != std::string::npos);

    struct ArrCap : note::JsonSink {
        std::vector<std::string> items;
        void on_string(note::string_view, note::string_view v) override {
            items.emplace_back(v.data(), v.size());
        }
    } cap;
    auto err = note::sax_parse(text, cap);
    CHECK(err.empty());
    CHECK(cap.items == std::vector<std::string>{"a", "b", "c"});
}

TEST_CASE("SaxToTextSink reports overflow on undersized buffer") {
    char buf[16];
    SaxToTextSink sink;
    sink.rearm(note::span<char>(buf, sizeof(buf)));

    sink.on_object_begin("");
    sink.on_string("a_long_key_name", "a_long_value_string");
    sink.on_object_end("");

    CHECK(sink.overflow());
}

TEST_CASE("SaxToTextSink rearm() resets state for a fresh response") {
    char buf[128];
    SaxToTextSink sink;
    sink.rearm(note::span<char>(buf, sizeof(buf)));
    sink.on_object_begin("");
    sink.on_int("x", 1);
    sink.on_object_end("");
    std::string first(sink.view());

    sink.rearm(note::span<char>(buf, sizeof(buf)));
    sink.on_object_begin("");
    sink.on_int("y", 2);
    sink.on_object_end("");
    std::string second(sink.view());

    CHECK(first.find("\"x\":1") != std::string::npos);
    CHECK(second.find("\"y\":2") != std::string::npos);
    CHECK(second.find("\"x\":") == std::string::npos);
}
