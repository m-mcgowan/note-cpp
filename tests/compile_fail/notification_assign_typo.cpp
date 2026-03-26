// Compile-fail test: assignment of invalid flag string literal.
// On C++20 + GCC: consteval validates and rejects "typo".
// Currently PASSES (bug) — FlagSet is non-copyable, blocking the
// consteval assignment pattern.
#if defined(__clang__)
#error "Skipped on Clang (consteval + optional materialization bug)"
#endif
#include <note/api/card_aux_serial.hpp>
void test() {
    note::api::CardAuxSerial::Notify req;
    req.notifications = "typo";  // should fail
}
