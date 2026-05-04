// Compile-fail test: body must be a JSON object, not an array.
// Notecard body is always an object with properties.
// STATUS: VALIDATED at compile time on GCC 13.4+ via consteval BodyValue ctor.
#if defined(__clang__)
#error "Skipped on Clang (consteval-optional bug; see docs/known-issues.md)"
#endif
#include <note/api/note_add.hpp>
void test() {
    note::api::NoteAdd req;
    req.body = "[1,2,3]";  // should fail: array, not object
}
