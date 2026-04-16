// Compile check: the library must compile cleanly with NOTE_NO_BUFFERED.
// Verifies that the typed API (execute, response fields, body structs)
// works without the buffered path (no request() lambda, no transact(),
// no JsonReader tree access).

#define NOTE_NO_BUFFERED 1
#include <note/api.hpp>

void test() {
    // Typed request builds without the buffered path
    note::api::HubSet req;
    req.product = "com.example.app";
    req.mode = "periodic";

    // Intent-scoped request builds without the buffered path
    note::api::CardAttn::Arm arm;
    arm.seconds = 120;

    // Response types are available
    note::api::CardVersion::Response rsp{};
    auto ver = rsp.version;
    (void)ver;
}
