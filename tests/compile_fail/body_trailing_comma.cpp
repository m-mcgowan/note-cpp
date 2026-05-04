// Compile-fail test: body with trailing comma is invalid JSON.
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
    req.body = R"({"temp":22.5,})";  // should fail: trailing comma
}
