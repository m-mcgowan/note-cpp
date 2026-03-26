// Compile-fail test: body must be a JSON object, not an array.
// Notecard body is always an object with properties.
// STATUS: CURRENTLY COMPILES (not yet validated).
#if defined(__clang__)
#error "Skipped on Clang (consteval bug)"
#endif
#include <note/api/note_add.hpp>
void test() {
    note::api::NoteAdd req;
    req.body = "[1,2,3]";  // should fail: array, not object
}
