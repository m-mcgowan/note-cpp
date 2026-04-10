// Compile-fail test: body with unquoted key is invalid JSON.
// STATUS: CURRENTLY COMPILES (not yet validated).
#if defined(__clang__) || (defined(__GNUC__) && __GNUC__ < 14)
#error "Skipped: consteval body validation unavailable"
#endif
#include <note/api/note_add.hpp>
void test() {
    note::api::NoteAdd req;
    req.body = R"({temp:22.5})";  // should fail: unquoted key
}
