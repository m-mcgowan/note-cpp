// Compile-fail: rearm is a mode action, not a combinable trigger flag.
#include <note/api/card_attn.hpp>
void test() {
    uint32_t f = note::attn::rearm;  // should fail
    (void)f;
}
