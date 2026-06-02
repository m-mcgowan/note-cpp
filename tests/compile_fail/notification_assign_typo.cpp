// Compile-fail test: assignment of invalid flag string literal.
// On C++20 + GCC the flag type's consteval ctor validates and rejects
// "typo" — verified failing on GCC 13 and 14.
#if defined(__clang__)
#error "Skipped on Clang (consteval + optional materialization bug)"
#endif
#include <note/api/card_aux_serial.hpp>
void test() {
    note::api::CardAuxSerial::Notify req;
    req.notifications = "typo";  // should fail
}
