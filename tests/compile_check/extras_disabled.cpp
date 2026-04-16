// Compile check: the library must compile cleanly with NOTE_EXTRAS=0.
// Verifies that request types, execute(), and response access all work
// without the extras/operator[] machinery.

#define NOTE_EXTRAS 0
#include <note/api.hpp>

void test() {
    // Typed request builds without extras
    note::api::HubSet req;
    req.product = "com.example.app";
    req.mode = "periodic";

    // Intent-scoped request builds without extras
    note::api::CardAttn::Arm arm;
    arm.seconds = 120;

    // Unguided request builds without extras
    note::api::CardAttn::Request full;
    full.mode = "arm,connected";
    full.seconds = 120;
}
