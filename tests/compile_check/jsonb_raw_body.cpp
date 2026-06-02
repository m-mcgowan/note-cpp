// Compile-check: raw JSON string body literals compile under NOTE_JSONB
// (when NOTE_MINIMAL is not set — see docs/known-issues.md for the AVR
// carve-out). The JSONB builder SAX-parses the literal and re-encodes
// it as opcodes, so the same surface that works under JSON works here.
#define NOTE_JSONB 1
#include <note/api/note_add.hpp>
#if !NOTE_MINIMAL
void test() {
    note::api::NoteAdd req;
    req.body = R"({"temp":22.5})";  // expected: compiles cleanly
}
#endif
