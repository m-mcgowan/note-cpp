// Compile-fail test: card.aux.serial gps() should not have env() method.
// Expected: compilation error — Gps struct has no member env().
#include <note/api/card_aux_serial.hpp>
void test() {
    note::api::CardAuxSerial::Gps req;
    req.env();  // should fail: env() is only on Notify
}
