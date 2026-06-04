// Compile-fail test: the `.with(...)` arg count must match the template's
// slot count. Expected error: the requires clause
// `sizeof...(Args) == sizes_.slot_count` removes the overload, so the call
// fails with "no matching member function" rather than a static_assert.
#include <note/body_template.hpp>
void test() {
    constexpr auto tpl = note::body_template<R"({"a":$1,"b":$2})">();
    (void)tpl.with(1);  // template has 2 slots, only 1 arg provided
}
