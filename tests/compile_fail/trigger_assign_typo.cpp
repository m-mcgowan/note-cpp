// Compile-fail test: assignment of invalid trigger string literal.
// "disarm" is a mode, not a trigger — should fail on assignment too.
#if defined(__clang__)
#error "Skipped on Clang (consteval + optional materialization bug)"
#endif
#include <note/api/card_attn.hpp>
void test() {
    note::api::CardAttn::Arm req;
    req.triggers = "disarm";  // should fail
}
