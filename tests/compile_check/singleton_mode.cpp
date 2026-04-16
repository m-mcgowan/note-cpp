// Compile check: the library must compile cleanly with NOTE_SINGLETON=1.
// Verifies that the singleton Api pattern (static Notecard pointer,
// static thunks) compiles and the API surface is intact.

#define NOTE_SINGLETON 1
#include <note/api.hpp>

void test() {
    // Typed request builds in singleton mode
    note::api::HubSet req;
    req.product = "com.example.app";

    // Intent-scoped request builds in singleton mode
    note::api::CardAttn::Arm arm;
    arm.seconds = 120;

    // Response types are available
    note::api::CardVersion::Response rsp{};
    auto ver = rsp.version;
    (void)ver;
}
