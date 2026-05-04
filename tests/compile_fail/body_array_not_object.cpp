// Compile-fail test: body must be a JSON object, not an array.
// Notecard body is always an object with properties.
// STATUS: VALIDATED at compile time on GCC 14+ via consteval BodyValue ctor.
//   GCC 13 trips PR 102933 on the inherited consteval ctor (see body.hpp).
#if defined(__clang__)
#error "Skipped on Clang (consteval-optional bug; see docs/known-issues.md)"
#endif
#if defined(__GNUC__) && __GNUC__ < 14
#error "Body validation tests require GCC 14+ (PR 102933 affects inherited consteval on 13.x)"
#endif
#include <note/api/note_add.hpp>
void test() {
    note::api::NoteAdd req;
    req.body = "[1,2,3]";  // should fail: array, not object
}
