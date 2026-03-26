// Compile-fail: card.attn sleep intent should not have triggers field.
#include <note/api/card_attn.hpp>
void test() {
    note::api::CardAttn::Sleep req;
    req.triggers.connected();  // should fail: Sleep has no triggers
}
