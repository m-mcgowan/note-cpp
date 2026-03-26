// Compile-fail test: body must be a JSON object, not a primitive.
// STATUS: CURRENTLY COMPILES (not yet validated).
#if defined(__clang__)
#error "Skipped on Clang (consteval bug)"
#endif
#include <note/api/note_add.hpp>
void test() {
    note::api::NoteAdd req;
    req.body = "42";  // should fail: primitive, not object
}
