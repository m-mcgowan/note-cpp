// .assume() is the target-widening escape hatch: it returns a temporary Api
// typed to the asserted axes (replacement, not intersection), so a call that
// would warn under the declared target compiles clean when wrapped through
// an assumed axis that does support the endpoint.
//
// Declared target = LoRa (doesn't support card.sleep, which is WiFi-only).
// Assumed target  = NOTE_ESP (WiFi family) → card.sleep is valid.
// Compile-check runs under `-Werror=deprecated-declarations`, so a stray
// deprecation on the assumed call would fail the test.
#include <note/api.hpp>
#include <note/sku_info.hpp>

using LoRaTarget = note::Target<note::Radios::LoRa>;

void test(note::Api<LoRaTarget>& api) {
    api.assume(note::sku::NOTE_ESP).card.sleep();
}
