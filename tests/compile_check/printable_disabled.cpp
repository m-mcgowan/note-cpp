// Compile check: the library must compile cleanly with NOTE_PRINTABLE=0.
// Verifies that requests, responses, and error handling work without
// the Arduino Printable vtable machinery.

#define NOTE_PRINTABLE 0
#include <note/api.hpp>
#include <note/error.hpp>

void test() {
    note::api::HubSet req;
    req.product = "com.example.app";

    note::api::CardVersion::Response rsp{};
    auto ver = rsp.version;
    (void)ver;

    note::ErrorInfo err{note::Error::Notecard, "test"};
    auto code = err.code;
    (void)code;
}
