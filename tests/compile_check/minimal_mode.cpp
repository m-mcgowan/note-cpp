// Compile check: the library must compile cleanly with NOTE_MINIMAL.
// NOTE_MINIMAL sets NOTE_EXTRAS=0, NOTE_PRINTABLE=0, NOTE_NO_JSON_TREE,
// NOTE_NO_CRC, NOTE_NO_MD5, NOTE_NO_STD_STRING, NOTE_NO_RETRY,
// NOTE_NO_REQUEST_IDS, and NOTE_JSONB=1. This verifies the reduced API
// surface compiles as a standalone check.

#define NOTE_MINIMAL 1
#include <note/api.hpp>

void test() {
    // Typed request builds under NOTE_MINIMAL
    note::api::HubSet req;
    req.product = "com.example.app";

    // Intent-scoped request builds under NOTE_MINIMAL
    note::api::CardAttn::Arm arm;
    arm.seconds = 120;

    // Response types are available
    note::api::CardVersion::Response rsp{};
    auto ver = rsp.version;
    (void)ver;
}
