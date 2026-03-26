// Compile-fail: consteval validates each token in comma-separated flags.
#if defined(__clang__)
#error "Skipped on Clang (consteval + optional materialization bug)"
#endif// "env" is valid but "typo" is not.
#include <note/api/card_aux_serial.hpp>
void test() {
    note::api::CardAuxSerial::Notify::notifications_t n = "env,typo";
    (void)n;
}
