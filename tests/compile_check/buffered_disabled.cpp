// Compile check: the library must compile cleanly with NOTE_NO_JSON_TREE.
// Verifies that the typed API (execute, response fields, body structs)
// works without the JSON tree-mode path (no JsonReader access via body(),
// no body_or_error(), no parse(reader_) factory).

#define NOTE_NO_JSON_TREE 1
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
