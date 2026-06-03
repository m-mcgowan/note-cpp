// Compile-fail test: passing a primitive to a `$No` object slot must reject.
// Object/array slots ($No / $Na) require a jsonb_body / jsonb_array argument
// (anything exposing emit_to). Expected error: static_assert in convert_arg_
// ("object/array slot ($No / $Na) requires a jsonb_body or jsonb_array
// argument").
#include <note/body_template.hpp>
void test() {
    constexpr auto tpl = note::experimental::body_template<R"({"x":$1o})">();
    (void)tpl.with(42);  // int in object slot — should fail
}
