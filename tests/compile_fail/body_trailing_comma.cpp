// Compile-fail test: body with trailing comma is invalid JSON.
// STATUS: VALIDATED at compile time on GCC 13.4+ via consteval BodyValue ctor.
#if defined(__clang__)
#error "Skipped on Clang (consteval-optional bug; see docs/known-issues.md)"
#endif
#include <note/api/note_add.hpp>
void test() {
    note::api::NoteAdd req;
    req.body = R"({"temp":22.5,})";  // should fail: trailing comma
}
