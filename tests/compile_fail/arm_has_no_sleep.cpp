// Compile-fail: arm triggers should not have sleep (it's a mode, not a trigger).
#include <note/api/card_attn.hpp>
void test() {
    note::api::CardAttn::Arm req;
    req.triggers.sleep();  // should fail
}
