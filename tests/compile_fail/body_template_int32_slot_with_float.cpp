// Compile-fail test: passing a float to a `$N` int32 slot must reject.
// The slot type is fixed at template-parse time; the runtime argument's type
// must match. Expected error: static_assert in patch_one() ("int32 slot ($N)
// requires an integral argument").
#include <note/body_template.hpp>
void test() {
    constexpr auto tpl = note::body_template<R"({"a":$1})">();
    (void)tpl.with(3.14);  // float in int32 slot — should fail
}
