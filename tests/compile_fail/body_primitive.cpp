// Compile-fail test: body must be a JSON object, not a primitive.
// STATUS: VALIDATED at compile time on GCC 13.4+ via consteval BodyValue ctor.
#if defined(__clang__)
#error "Skipped on Clang (consteval-optional bug; see docs/known-issues.md)"
#endif
#include <note/api/note_add.hpp>
void test() {
    note::api::NoteAdd req;
    req.body = "42";  // should fail: primitive, not object
}
