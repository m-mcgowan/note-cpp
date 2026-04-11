// Compile-fail test: raw JSON string body is not supported under JSONB.
// Use body() with a lambda or typed struct instead.
#define NOTE_JSONB 1
#include <note/api/note_add.hpp>
void test() {
    note::api::NoteAdd req;
    req.body = R"({"temp":22.5})";  // should fail: raw string body + JSONB
}
