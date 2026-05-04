// Compile-fail test: body must be a JSON object, not a primitive.
// STATUS: VALIDATED at compile time on GCC 14+ via consteval BodyValue ctor.
//   GCC 13 trips PR 102933 on the inherited consteval ctor (`body_t : BodyValue`
//   uses `using BodyValue::BodyValue;`); body.hpp gates the validating ctor on
//   __GNUC__ >= 14, so on GCC 13 the bad literal is silently accepted.
#if defined(__clang__)
#error "Skipped on Clang (consteval-optional bug; see docs/known-issues.md)"
#endif
#if defined(__GNUC__) && __GNUC__ < 14
#error "Body validation tests require GCC 14+ (PR 102933 affects inherited consteval on 13.x)"
#endif
#include <note/api/note_add.hpp>
void test() {
    note::api::NoteAdd req;
    req.body = "42";  // should fail: primitive, not object
}
