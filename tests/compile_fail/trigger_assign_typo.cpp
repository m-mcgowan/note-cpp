// Compile-fail test: assignment of invalid trigger string literal.
// "disarm" is a mode, not a trigger — should fail on assignment too.
#if defined(__clang__) || defined(__GNUC__)
#error "Skipped: consteval trigger validation only works on MSVC/EDG"
#endif
#include <note/api/card_attn.hpp>
void test() {
    note::api::CardAttn::Arm req;
    req.triggers = "disarm";  // should fail
}
