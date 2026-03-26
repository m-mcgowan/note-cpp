// Compile-fail: consteval validates flag string literals on C++20.
#if defined(__clang__)
#error "Skipped on Clang (consteval + optional materialization bug)"
#endif// "typo" is not a valid notification flag.
#include <note/api/card_aux_serial.hpp>
void test() {
    note::api::CardAuxSerial::Notify::notifications_t n = "typo";
    (void)n;
}
