// Compile-fail: consteval validates each token in comma-separated flags.
// "env" is valid but "typo" is not.
#include <note/api/card_aux_serial.hpp>
void test() {
    note::api::CardAuxSerial::Notify::notifications_t n = "env,typo";
    (void)n;
}
