// Compile-fail test: passing a primitive to a `$No` object slot must reject.
// Object/array slots ($No / $Na) require a body_object / body_array argument
// (anything exposing emit_to). Expected error: static_assert in convert_arg_
// ("object/array slot ($No / $Na) requires a body_object or body_array
// argument").
#include <note/body_template.hpp>
void test() {
    constexpr auto tpl = note::body_template<R"({"x":$1o})">();
    (void)tpl.with(42);  // int in object slot — should fail
}
