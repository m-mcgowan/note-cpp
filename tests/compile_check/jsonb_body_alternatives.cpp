// Compile-check: lambda and typed struct bodies work under NOTE_JSONB.
// These shapes never go through add_raw, so they avoid pulling the SAX
// lexer into the build path that raw string bodies use.
//
// Each shape is shown twice: once with literal values, and once with
// runtime-supplied values, to demonstrate how dynamic data enters the
// body. Raw string bodies (`req.body = R"(...)"`) carry no runtime
// substitution — they're for compile-time-fixed JSON only; reach for
// the lambda or struct shape when any field varies at runtime.
#define NOTE_JSONB 1
#include <note/api/note_add.hpp>
#include <note/body.hpp>

#include <cstdint>
#include <cstdlib>   // std::rand

struct Readings {
    float temperature;
    int32_t humidity;
    int32_t sequence;
    NOTE_FIELDS(temperature, humidity, sequence)
};

// Static lambda body — equivalent to a raw string literal, but without
// the SAX-replay cost on JSONB builds.
void test_lambda_body_static() {
    note::api::NoteAdd req;
    req.file = "sensors.qo";
    req.body(note::body([](note::JsonBuilder& b) {
        b.add("temperature", 22.5);
        b.add("humidity", int32_t{60});
    }));
}

// Lambda body with runtime values — the closure captures by reference,
// so each field can be a runtime expression. This is the shape to
// reach for when "build a JSON body with a random integer" or any
// other dynamic data needs to land on the wire.
void test_lambda_body_runtime(float measured_temp, int32_t measured_humidity) {
    int32_t sequence = std::rand();
    note::api::NoteAdd req;
    req.file = "sensors.qo";
    req.body(note::body([&](note::JsonBuilder& b) {
        b.add("temperature", static_cast<double>(measured_temp));
        b.add("humidity", measured_humidity);
        b.add("sequence", sequence);
    }));
}

// Typed-struct body, fixed values.
void test_typed_struct_body_static() {
    note::api::NoteAdd req;
    req.file = "sensors.qo";
    req.body(Readings{22.5f, 60, 0});
}

// Typed-struct body with runtime values — the struct is built normally
// in C++ and serialised field-by-field via StructSink. NOTE_FIELDS
// enumerates the fields the library should walk.
void test_typed_struct_body_runtime(float measured_temp, int32_t measured_humidity) {
    Readings r{
        .temperature = measured_temp,
        .humidity    = measured_humidity,
        .sequence    = std::rand(),
    };
    note::api::NoteAdd req;
    req.file = "sensors.qo";
    req.body(r);
}
