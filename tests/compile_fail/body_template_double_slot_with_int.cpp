// Compile-fail test: passing an int to a `$Nf` double slot must reject.
// Expected error: static_assert in patch_one() ("double slot ($Nf) requires
// a floating-point argument").
#include <note/body_template.hpp>
void test() {
    constexpr auto tpl = note::experimental::body_template<R"({"t":$1f})">();
    (void)tpl.with(42);  // int in double slot — should fail
}
