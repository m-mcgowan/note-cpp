// Compile-fail test: passing an int to a `$Nb` bool slot must reject.
// We deliberately disallow implicit int→bool conversion at the slot boundary —
// the template's safety contract is that arg types name their intent. A 0/1
// int reaching a bool slot is almost always a bug; if the caller really means
// it, they can `static_cast<bool>(...)`. Expected error: static_assert in
// patch_one() ("bool slot ($Nb) requires a bool argument").
#include <note/body_template.hpp>
void test() {
    constexpr auto tpl = note::body_template<R"({"on":$1b})">();
    (void)tpl.with(1);  // int in bool slot — should fail
}
