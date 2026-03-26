// Compile-fail test: card.attn arm triggers should not have disarm().
// Expected: compilation error — disarm is a mode, not a trigger flag.
#include <note/api/card_attn.hpp>
void test() {
    note::api::CardAttn::Arm req;
    req.triggers.disarm();  // should fail: disarm not in arm's flags
}
