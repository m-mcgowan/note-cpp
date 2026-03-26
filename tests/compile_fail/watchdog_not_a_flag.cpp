// Compile-fail test: watchdog is a mode (separate intent), not a trigger flag.
// It should not be available as a flag constant or method on triggers.
#include <note/api/card_attn.hpp>
void test() {
    // watchdog should not exist as a flag constant
    uint32_t f = note::attn::watchdog;  // should fail
    (void)f;
}
