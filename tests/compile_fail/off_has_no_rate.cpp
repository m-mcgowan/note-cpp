// Compile-fail test: card.aux.serial off() should not have rate().
// Expected: compilation error — Off has no fields.
#include <note/api/card_aux_serial.hpp>
void test() {
    note::api::CardAuxSerial::Off req;
    req.rate = 9600;  // should fail: Off has no rate field
}
