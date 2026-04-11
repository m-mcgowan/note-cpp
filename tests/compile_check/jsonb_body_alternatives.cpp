// Compile-check: lambda and typed struct bodies work under NOTE_JSONB.
// These are the recommended alternatives to raw JSON string bodies.
#define NOTE_JSONB 1
#include <note/api/note_add.hpp>
#include <note/body.hpp>

struct Readings {
    float temperature;
    int32_t humidity;
    NOTE_FIELDS(temperature, humidity)
};

void test_lambda_body() {
    note::api::NoteAdd req;
    req.file = "sensors.qo";
    req.body(note::body([](note::JsonBuilder& b) {
        b.add("temperature", 22.5);
        b.add("humidity", int32_t{60});
    }));
}

void test_typed_struct_body() {
    note::api::NoteAdd req;
    req.file = "sensors.qo";
    req.body(Readings{22.5f, 60});
}
