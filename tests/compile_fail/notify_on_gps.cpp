// Compile-fail: gps intent should not have notification flag methods.
#include <note/api/card_aux_serial.hpp>
void test() {
    note::api::CardAuxSerial::Gps req;
    req.dfu();  // should fail
}
