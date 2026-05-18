// Compile check: the deprecated NOTE_NO_BUFFERED still works as an alias
// for NOTE_NO_JSON_TREE. Existing user build_flags must not break when the
// canonical macro is renamed.

#define NOTE_NO_BUFFERED 1
#include <note/api.hpp>

#if !NOTE_NO_JSON_TREE
#error "NOTE_NO_BUFFERED=1 must imply NOTE_NO_JSON_TREE=1 via the back-compat shim"
#endif

void test() {
    note::api::HubSet req;
    req.product = "com.example.app";
}
