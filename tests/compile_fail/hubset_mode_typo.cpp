// Compile-fail test: hub.set mode assignment with invalid string literal.
// C++20 + GCC: consteval validates and rejects "perioidc".
// Clang: skipped — Apple Clang consteval + optional materialization bug.
#if defined(__clang__)
#error "Skipped on Clang (consteval bug — see TODO)"
#endif
#include <note/api/hub_set.hpp>
void test() {
    note::api::HubSet req;
    req.mode = "perioidc";  // should fail: not a valid mode
}
