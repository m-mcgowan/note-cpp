// Compile-fail: sleep is a mode (separate intent), not a combinable trigger flag.
#include <note/api/card_attn.hpp>
void test() {
    uint32_t f = note::attn::sleep;  // should fail
    (void)f;
}
