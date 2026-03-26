// Compile-fail: consteval validates trigger string literals on C++20.
// "disarm" is not a valid trigger flag (it's a mode).
#include <note/api/card_attn.hpp>
void test() {
    note::api::CardAttn::Arm::triggers_t t = "disarm";
    (void)t;
}
