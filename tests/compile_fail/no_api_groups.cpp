// Compile-fail: NOTE_NO_API_GROUPS should prevent Api from compiling.
// The static_assert inside Api fires when this flag is set.

#define NOTE_NO_API_GROUPS 1
#include <note/api.hpp>

void test() {
    note::Notecard nc;
    note::Api api(nc);  // should fail: Api groups disabled
    api.card.temp();
}
